/*-------------------------------------------------------------------------
 *
 * pagestore.c
 *	  Copy-on-write storage manager for permanent relations.
 *
 * The page store keeps each relation fork as a stack of layers rather than
 * a single file.  A layer is a directory pg_pagestore/<generation>/ holding,
 * for each fork that changed while the layer was writable:
 *
 *	-- a data file "<spc>_<db>_<relnumber>_<fork>", in which block N lives at
 *	   offset N * BLCKSZ, as in an md.c file.  Blocks the layer never wrote
 *	   are holes.
 *	-- optionally a map file, the data file's name followed by ".map".  A map
 *	   either says the fork was dropped, hiding the layers below, or records
 *	   a "base": blocks below base are inherited from the layers below,
 *	   except those the map's bitmap says this layer wrote.
 *
 * A fork with a data file and no map has base 0: the layer owns all of it.
 *
 * Block N of a fork lives in the topmost layer that has a data file for the
 * fork and either N >= base or N set in its bitmap.  A layer without files
 * for the fork is transparent.  The size of a fork is the size of its data
 * file in the topmost layer that has one, and base never exceeds it.
 *
 * Only the top layer is written.  PageStoreFreeze() makes the top layer
 * immutable and starts a new, empty one; that costs time proportional to
 * the number of forks written since the last freeze, not to their size.  A
 * frozen layer never changes again, so another data directory can share it
 * by hard link.  That is how pg_branch() (see pagestore_branch.c) copies a
 * cluster in time independent of its size.
 *
 * The top layer's metadata lives in shared memory: a hash table of the forks
 * that have files in the top layer, and a hash table of 1 kB bitmap chunks.
 * Both are loaded from the top layer's files when shared memory is set up,
 * and both are protected by PS_NUM_PARTITIONS partition locks, chosen by the
 * fork.  A map file is written when a checkpoint syncs its fork, with the
 * bits that were set before the data file was fsync'd, so a bit never
 * becomes durable before the block it describes.  Maps are written durably
 * right away when base shrinks or a fork over lower layers is dropped.
 * Frozen layers' metadata is read into backend-local memory on first use.
 *
 * Every change to the top layer happens under the freeze lock, held in
 * shared mode, so a freeze sees no change in progress.  Readers don't take
 * it: a freeze moves no block, it only stops writes into the old top layer.
 *
 * Temporary relations, and relations in tablespaces other than pg_default
 * and pg_global, stay with md.c.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/pagestore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/falloc.h>
#endif

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/pg_tablespace_d.h"
#include "commands/tablespace.h"
#include "common/file_utils.h"
#include "common/hashfn.h"
#include "common/relpath.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "postmaster/bgwriter.h"
#include "port/pg_iovec.h"
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/pagestore.h"
#include "storage/reinit.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/* GUCs */
int			pagestore_max_forks = 65536;
int			pagestore_max_bitmap_chunks = 16384;
bool		pagestore_reclaim = true;

#define PS_NUM_PARTITIONS	16

/* A bitmap chunk covers this many blocks */
#define PS_CHUNK_BLOCKS		8192
#define PS_CHUNK_WORDS		(PS_CHUNK_BLOCKS / 32)

#define PS_MAP_SUFFIX		".map"
#define PS_TMP_SUFFIX		".tmp"
#define PS_MAP_MAGIC		0x50534D50	/* "PSMP" */
#define PS_MAP_VERSION		1
#define PS_MAP_DROPPED		0x0001

/* A relation fork, as it is named in a layer */
typedef struct PsForkKey
{
	RelFileLocator rlocator;
	int32		forknum;
} PsForkKey;

typedef enum PsForkKind
{
	PS_FORK_DATA,				/* the layer has a data file for the fork */
	PS_FORK_DROPPED,			/* the fork was dropped in the layer */
} PsForkKind;

/* A fork with files in the top layer, in shared memory */
typedef struct PsTopFork
{
	PsForkKey	key;			/* hash key, must be first */
	uint8		kind;			/* a PsForkKind */
	bool		has_map;		/* the fork has a map file */
	bool		map_dirty;		/* bits or base changed since it was written */
	bool		unsynced;		/* written since its data file was fsync'd */
	uint32		map_version;	/* bumped whenever map_dirty is set */
	BlockNumber base;
} PsTopFork;

typedef struct PsChunkKey
{
	PsForkKey	fork;
	uint32		chunkno;
} PsChunkKey;

/* PS_CHUNK_BLOCKS bits of a top layer fork's bitmap, in shared memory */
typedef struct PsChunk
{
	PsChunkKey	key;			/* hash key, must be first */
	uint32		bits[PS_CHUNK_WORDS];
} PsChunk;

/* A map file starts with this, followed by (base + 31) / 32 bitmap words */
typedef struct PsMapHeader
{
	uint32		magic;
	uint32		version;
	uint32		flags;
	BlockNumber base;
	pg_crc32c	crc;			/* of the header up to here, then the bits */
} PsMapHeader;

typedef struct PsShared
{
	bool		enabled;

	/*
	 * The stack, bottom first.  Changed only by a freeze, while holding all
	 * partition locks, so holding any one of them is enough to read it.
	 */
	uint32		stack_version;
	int			nlayers;
	uint32		layers[PAGESTORE_MAX_LAYERS];

	/* WAL from here on is being copied into a branch, see KeepLogSeg() */
	pg_atomic_uint64 branch_keep_lsn;

	/* set while PageStoreReclaim() runs */
	pg_atomic_flag reclaim_running;

	LWLock		freeze_lock;
	LWLock		branch_lock;
	LWLockPadded partition_locks[PS_NUM_PARTITIONS];
} PsShared;

static PsShared *PsCtl = NULL;
static HTAB *PsTopForks = NULL;
static HTAB *PsChunks = NULL;

/* A frozen layer's metadata for a fork, in backend-local memory */
typedef struct PsFrozenKey
{
	uint32		gen;
	PsForkKey	fork;
} PsFrozenKey;

typedef struct PsFrozenFork
{
	PsFrozenKey key;			/* hash key, must be first */
	bool		has_data;
	bool		has_map;
	bool		loaded;			/* the fields below are filled in */
	bool		absent;			/* its files were reclaimed since */
	uint8		kind;
	BlockNumber base;
	BlockNumber nblocks;
	uint32	   *bits;
} PsFrozenFork;

/* A file a backend has open, for one layer of a fork */
typedef struct PsOpenFile
{
	uint32		gen;
	File		vfd;
} PsOpenFile;

static MemoryContext PsCxt = NULL;
static HTAB *ps_frozen = NULL;	/* PsFrozenFork, by generation and fork */
static HTAB *ps_indexed_gens = NULL;	/* frozen layers read into ps_frozen */

/* Backend-local copy of the stack */
static uint32 ps_local_stack_version = 0;
static int	ps_local_nlayers = 0;
static uint32 ps_local_layers[PAGESTORE_MAX_LAYERS];

#define PS_TOPGEN()		(ps_local_layers[ps_local_nlayers - 1])

static void PageStoreShmemRequest(void *arg);
static void PageStoreShmemInit(void *arg);

const ShmemCallbacks PageStoreShmemCallbacks = {
	.request_fn = PageStoreShmemRequest,
	.init_fn = PageStoreShmemInit,
};

/* Populate a file tag describing one layer's files for a fork. */
#define INIT_PS_FILETAG(a,xx_rlocator,xx_forknum,xx_gen) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = SYNC_HANDLER_PAGESTORE, \
	(a).rlocator = (xx_rlocator), \
	(a).forknum = (xx_forknum), \
	(a).segno = (xx_gen) \
)


/*
 * Names and paths
 */

static inline void
ps_key_init(PsForkKey *key, RelFileLocator rlocator, ForkNumber forknum)
{
	memset(key, 0, sizeof(PsForkKey));
	key->rlocator = rlocator;
	key->forknum = forknum;
}

static void
ps_data_path(char *path, uint32 gen, const PsForkKey *key)
{
	snprintf(path, MAXPGPATH, "%s/%u/%u_%u_%u_%s", PAGESTORE_DIR, gen,
			 key->rlocator.spcOid, key->rlocator.dbOid,
			 key->rlocator.relNumber, forkNames[key->forknum]);
}

static void
ps_map_path(char *path, uint32 gen, const PsForkKey *key)
{
	ps_data_path(path, gen, key);
	strlcat(path, PS_MAP_SUFFIX, MAXPGPATH);
}

static void
ps_layer_path(char *path, uint32 gen)
{
	snprintf(path, MAXPGPATH, "%s/%u", PAGESTORE_DIR, gen);
}

/*
 * Parse a file name in a layer.  Returns false if it's not a data or map
 * file; *is_tmp is set for leftover temporary files.
 */
static bool
ps_parse_name(const char *name, PsForkKey *key, bool *is_map, bool *is_tmp)
{
	unsigned int spc,
				db,
				rel;
	int			pos = 0;
	const char *rest;

	*is_map = false;
	*is_tmp = false;

	if (sscanf(name, "%u_%u_%u_%n", &spc, &db, &rel, &pos) != 3 || pos == 0)
		return false;

	rest = name + pos;
	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		size_t		len = strlen(forkNames[forknum]);

		if (strncmp(rest, forkNames[forknum], len) != 0)
			continue;

		rest += len;
		if (strcmp(rest, PS_MAP_SUFFIX) == 0)
			*is_map = true;
		else if (strcmp(rest, PS_MAP_SUFFIX PS_TMP_SUFFIX) == 0)
			*is_tmp = true;
		else if (*rest != '\0')
			return false;

		{
			RelFileLocator rlocator;

			rlocator.spcOid = spc;
			rlocator.dbOid = db;
			rlocator.relNumber = rel;
			ps_key_init(key, rlocator, forknum);
		}
		return true;
	}
	return false;
}

static inline int
ps_open_flags(void)
{
	int			flags = O_RDWR | PG_BINARY;

	if (io_direct_flags & IO_DIRECT_DATA)
		flags |= PG_O_DIRECT;

	return flags;
}


/*
 * Shared memory
 */

static inline uint32
ps_fork_hash(const PsForkKey *key)
{
	return hash_bytes((const unsigned char *) key, sizeof(PsForkKey));
}

static inline LWLock *
ps_partition_lock(uint32 hash)
{
	return &PsCtl->partition_locks[hash % PS_NUM_PARTITIONS].lock;
}

/*
 * Chunks are protected by their fork's partition lock, so a chunk's hash
 * code must fall in the same dynahash partition as the fork's: keep the low
 * bits of the fork's hash code.
 */
static inline uint32
ps_chunk_hash(uint32 forkhash, uint32 chunkno)
{
	uint32		h = hash_combine(forkhash, hash_uint32(chunkno));

	return (h & ~(uint32) (PS_NUM_PARTITIONS - 1)) |
		(forkhash & (PS_NUM_PARTITIONS - 1));
}

static bool
ps_dir_exists(void)
{
	struct stat st;

	return stat(PAGESTORE_STACK_FILE, &st) == 0;
}

static void
PageStoreShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "Page Store Data",
					   .size = sizeof(PsShared),
					   .ptr = (void **) &PsCtl,
		);

	/* The hash tables are only needed by a cluster using the page store */
	if (!ps_dir_exists())
	{
		PsTopForks = NULL;
		PsChunks = NULL;
		return;
	}

	ShmemRequestHash(.name = "Page Store Top Layer Forks",
					 .nelems = pagestore_max_forks,
					 .ptr = &PsTopForks,
					 .hash_info.keysize = sizeof(PsForkKey),
					 .hash_info.entrysize = sizeof(PsTopFork),
					 .hash_info.num_partitions = PS_NUM_PARTITIONS,
					 .hash_flags = HASH_ELEM | HASH_BLOBS | HASH_PARTITION | HASH_FIXED_SIZE,
		);
	ShmemRequestHash(.name = "Page Store Top Layer Bitmaps",
					 .nelems = pagestore_max_bitmap_chunks,
					 .ptr = &PsChunks,
					 .hash_info.keysize = sizeof(PsChunkKey),
					 .hash_info.entrysize = sizeof(PsChunk),
					 .hash_info.num_partitions = PS_NUM_PARTITIONS,
					 .hash_flags = HASH_ELEM | HASH_BLOBS | HASH_PARTITION | HASH_FIXED_SIZE,
		);
}

static void ps_read_stack(uint32 *layers, int *nlayers);
static void ps_load_top_layer(void);

static void
PageStoreShmemInit(void *arg)
{
	memset(PsCtl, 0, sizeof(PsShared));
	pg_atomic_init_u64(&PsCtl->branch_keep_lsn, InvalidXLogRecPtr);
	pg_atomic_init_flag(&PsCtl->reclaim_running);
	LWLockInitialize(&PsCtl->freeze_lock, LWTRANCHE_PAGESTORE);
	LWLockInitialize(&PsCtl->branch_lock, LWTRANCHE_PAGESTORE);
	for (int i = 0; i < PS_NUM_PARTITIONS; i++)
		LWLockInitialize(&PsCtl->partition_locks[i].lock, LWTRANCHE_PAGESTORE);

	if (PsTopForks == NULL)
		return;

	PsCtl->enabled = true;
	ps_read_stack(PsCtl->layers, &PsCtl->nlayers);
	ps_load_top_layer();
}

bool
PageStoreEnabled(void)
{
	return PsCtl != NULL && PsCtl->enabled;
}

/*
 * Does the page store handle this relation, rather than md.c?
 */
bool
PageStoreHandlesRelation(RelFileLocator rlocator, ProcNumber backend)
{
	return PageStoreEnabled() &&
		backend == INVALID_PROC_NUMBER &&
		(rlocator.spcOid == DEFAULTTABLESPACE_OID ||
		 rlocator.spcOid == GLOBALTABLESPACE_OID);
}

/* Copy the stack to backend-local memory.  Caller holds a partition lock. */
static void
ps_sync_stack(void)
{
	if (ps_local_nlayers > 0 &&
		ps_local_stack_version == PsCtl->stack_version)
		return;

	ps_local_nlayers = PsCtl->nlayers;
	memcpy(ps_local_layers, PsCtl->layers,
		   sizeof(uint32) * ps_local_nlayers);
	ps_local_stack_version = PsCtl->stack_version;
}

static void
ps_acquire_all_partitions(LWLockMode mode)
{
	for (int i = 0; i < PS_NUM_PARTITIONS; i++)
		LWLockAcquire(&PsCtl->partition_locks[i].lock, mode);
}

static void
ps_release_all_partitions(void)
{
	for (int i = PS_NUM_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(&PsCtl->partition_locks[i].lock);
}


/*
 * The stack file
 */

static void
ps_read_stack(uint32 *layers, int *nlayers)
{
	char		buf[PAGESTORE_MAX_LAYERS * (OIDCHARS + 1) + 1];
	int			fd;
	int			len;
	char	   *p;

	fd = OpenTransientFile(PAGESTORE_STACK_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", PAGESTORE_STACK_FILE)));
	len = read(fd, buf, sizeof(buf) - 1);
	if (len < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", PAGESTORE_STACK_FILE)));
	CloseTransientFile(fd);
	buf[len] = '\0';

	*nlayers = 0;
	p = buf;
	for (;;)
	{
		char	   *end;
		unsigned long gen;

		while (*p == ' ' || *p == '\n')
			p++;
		if (*p == '\0')
			break;
		gen = strtoul(p, &end, 10);
		if (end == p || gen == 0 || gen > PG_UINT32_MAX ||
			*nlayers >= PAGESTORE_MAX_LAYERS ||
			(*nlayers > 0 && gen <= layers[*nlayers - 1]))
			ereport(FATAL,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid contents in file \"%s\"", PAGESTORE_STACK_FILE)));
		layers[(*nlayers)++] = (uint32) gen;
		p = end;
	}
	if (*nlayers == 0)
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid contents in file \"%s\"", PAGESTORE_STACK_FILE)));
}

/*
 * Durably write a stack file listing the given layers.  dir is the data
 * directory to write it in, or NULL for ours.
 */
void
PageStoreWriteStackFile(const char *dir, const uint32 *layers, int nlayers)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	StringInfoData buf;
	int			fd;

	if (dir)
		snprintf(path, sizeof(path), "%s/%s", dir, PAGESTORE_STACK_FILE);
	else
		strlcpy(path, PAGESTORE_STACK_FILE, sizeof(path));
	snprintf(tmppath, sizeof(tmppath), "%s%s", path, PS_TMP_SUFFIX);

	initStringInfo(&buf);
	for (int i = 0; i < nlayers; i++)
		appendStringInfo(&buf, "%u\n", layers[i]);

	fd = OpenTransientFile(tmppath, O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));
	errno = 0;
	if (write(fd, buf.data, buf.len) != buf.len)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmppath)));
	}
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));
	pfree(buf.data);

	/* fsyncs the file before the rename and the directory after it */
	durable_rename(tmppath, path, ERROR);
}


/*
 * Map files
 */

static void
ps_map_crc(const PsMapHeader *hdr, const uint32 *bits, size_t nwords,
		   pg_crc32c *crc)
{
	INIT_CRC32C(*crc);
	COMP_CRC32C(*crc, hdr, offsetof(PsMapHeader, crc));
	if (nwords > 0)
		COMP_CRC32C(*crc, bits, nwords * sizeof(uint32));
	FIN_CRC32C(*crc);
}

static inline size_t
ps_bitmap_words(BlockNumber base)
{
	return ((size_t) base + 31) / 32;
}

/*
 * Write a fork's map file in a layer.  bits has ps_bitmap_words(base) words,
 * or is NULL for none set.  Bits at or above base are ignored.  The map is
 * fsync'd and replaced atomically by rename; if durable, the directory is
 * fsync'd too, making the rename durable.  Failures are reported at elevel; returns false if one is
 * reported at a level below ERROR.
 */
static bool
ps_write_map(uint32 gen, const PsForkKey *key, uint32 flags,
			 BlockNumber base, const uint32 *bits, bool durable, int elevel)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	size_t		nwords = (flags & PS_MAP_DROPPED) ? 0 : ps_bitmap_words(base);
	size_t		len = sizeof(PsMapHeader) + nwords * sizeof(uint32);
	char	   *buf;
	PsMapHeader *hdr;
	uint32	   *outbits;
	int			fd;

	buf = MemoryContextAllocZero(PsCxt ? PsCxt : CurrentMemoryContext, len);
	hdr = (PsMapHeader *) buf;
	outbits = (uint32 *) (buf + sizeof(PsMapHeader));
	hdr->magic = PS_MAP_MAGIC;
	hdr->version = PS_MAP_VERSION;
	hdr->flags = flags;
	hdr->base = (flags & PS_MAP_DROPPED) ? 0 : base;
	if (bits && nwords > 0)
	{
		memcpy(outbits, bits, nwords * sizeof(uint32));
		if (base % 32 != 0)
			outbits[nwords - 1] &= (1U << (base % 32)) - 1;
	}
	ps_map_crc(hdr, outbits, nwords, &hdr->crc);

	ps_map_path(path, gen, key);
	snprintf(tmppath, sizeof(tmppath), "%s%s", path, PS_TMP_SUFFIX);

	fd = OpenTransientFile(tmppath, O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));
		pfree(buf);
		return false;
	}
	errno = 0;
	if (write(fd, buf, len) != len)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmppath)));
		CloseTransientFile(fd);
		pfree(buf);
		return false;
	}
	pfree(buf);

	/*
	 * Always fsync before the rename, so a crash leaves the old map or the
	 * new one, not an empty file.  durable adds the directory.
	 */
	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(elevel),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
		CloseTransientFile(fd);
		return false;
	}
	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));
		return false;
	}
	if (rename(tmppath, path) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));
		return false;
	}
	if (durable)
	{
		char		dir[MAXPGPATH];

		ps_layer_path(dir, gen);
		if (fsync_fname_ext(dir, true, false, data_sync_elevel(elevel)) != 0)
			return false;
	}
	return true;
}

/*
 * Read a map file.  Returns the bitmap (NULL if base is 0 or the fork was
 * dropped), allocated in mcxt.  If missing isn't NULL, a map that doesn't
 * exist sets it instead of raising an error.
 */
static uint32 *
ps_read_map(const char *path, uint32 *flags, BlockNumber *base,
			MemoryContext mcxt, bool *missing)
{
	PsMapHeader hdr;
	uint32	   *bits = NULL;
	size_t		nwords;
	pg_crc32c	crc;
	struct stat st;
	int			fd;

	if (missing)
		*missing = false;
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0 && missing && errno == ENOENT)
	{
		*missing = true;
		return NULL;
	}
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	if (fstat(fd, &st) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr) ||
		hdr.magic != PS_MAP_MAGIC || hdr.version != PS_MAP_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid page store map file \"%s\"", path)));

	nwords = (hdr.flags & PS_MAP_DROPPED) ? 0 : ps_bitmap_words(hdr.base);
	if (st.st_size != sizeof(hdr) + nwords * sizeof(uint32))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid page store map file \"%s\"", path)));
	if (nwords > 0)
	{
		bits = MemoryContextAlloc(mcxt, nwords * sizeof(uint32));
		if (read(fd, bits, nwords * sizeof(uint32)) != nwords * sizeof(uint32))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
	}
	CloseTransientFile(fd);

	ps_map_crc(&hdr, bits, nwords, &crc);
	if (!EQ_CRC32C(crc, hdr.crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("incorrect checksum in page store map file \"%s\"", path)));

	*flags = hdr.flags;
	*base = (hdr.flags & PS_MAP_DROPPED) ? 0 : hdr.base;
	return bits;
}


/*
 * Top layer metadata.  Callers hold the fork's partition lock: shared to
 * look, exclusive to change.
 */

static inline PsTopFork *
ps_top_lookup(const PsForkKey *key, uint32 hash)
{
	return (PsTopFork *) hash_search_with_hash_value(PsTopForks, key, hash,
													 HASH_FIND, NULL);
}

static PsTopFork *
ps_top_insert(const PsForkKey *key, uint32 hash)
{
	PsTopFork  *top;
	bool		found;

	top = (PsTopFork *) hash_search_with_hash_value(PsTopForks, key, hash,
													HASH_ENTER_NULL, &found);
	if (top == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of page store fork entries"),
				 errhint("You might need to increase \"%s\".",
						 "pagestore_max_forks")));
	if (!found)
	{
		top->kind = PS_FORK_DATA;
		top->has_map = false;
		top->map_dirty = false;
		top->unsynced = false;
		top->map_version = 0;
		top->base = 0;
	}
	return top;
}

static PsChunk *
ps_chunk_lookup(const PsForkKey *key, uint32 hash, uint32 chunkno,
				bool create)
{
	PsChunkKey	ckey;
	PsChunk    *chunk;
	bool		found;

	memset(&ckey, 0, sizeof(ckey));
	ckey.fork = *key;
	ckey.chunkno = chunkno;

	chunk = (PsChunk *) hash_search_with_hash_value(PsChunks, &ckey,
													ps_chunk_hash(hash, chunkno),
													create ? HASH_ENTER_NULL : HASH_FIND,
													&found);
	if (create)
	{
		if (chunk == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of page store bitmap chunks"),
					 errhint("You might need to increase \"%s\".",
							 "pagestore_max_bitmap_chunks")));
		if (!found)
			memset(chunk->bits, 0, sizeof(chunk->bits));
	}
	return chunk;
}

static bool
ps_top_bit(const PsForkKey *key, uint32 hash, BlockNumber blkno)
{
	PsChunk    *chunk = ps_chunk_lookup(key, hash, blkno / PS_CHUNK_BLOCKS, false);
	uint32		off = blkno % PS_CHUNK_BLOCKS;

	return chunk != NULL && (chunk->bits[off / 32] & (1U << (off % 32))) != 0;
}

static void
ps_top_set_bit(const PsForkKey *key, uint32 hash, BlockNumber blkno)
{
	PsChunk    *chunk = ps_chunk_lookup(key, hash, blkno / PS_CHUNK_BLOCKS, true);
	uint32		off = blkno % PS_CHUNK_BLOCKS;

	chunk->bits[off / 32] |= 1U << (off % 32);
}

/* Clear the bits at or above from, below the fork's old base. */
static void
ps_top_clear_bits(const PsForkKey *key, uint32 hash, BlockNumber from,
				  BlockNumber oldbase)
{
	for (uint32 chunkno = from / PS_CHUNK_BLOCKS;
		 (uint64) chunkno * PS_CHUNK_BLOCKS < oldbase;
		 chunkno++)
	{
		PsChunk    *chunk = ps_chunk_lookup(key, hash, chunkno, false);
		uint64		chunkstart = (uint64) chunkno * PS_CHUNK_BLOCKS;

		if (chunk == NULL)
			continue;
		if (chunkstart >= from)
		{
			hash_search_with_hash_value(PsChunks, &chunk->key,
										ps_chunk_hash(hash, chunkno),
										HASH_REMOVE, NULL);
			continue;
		}
		for (uint32 off = from - chunkstart; off < PS_CHUNK_BLOCKS; off++)
			chunk->bits[off / 32] &= ~(1U << (off % 32));
	}
}

static void
ps_top_remove(PsTopFork *top, uint32 hash)
{
	PsForkKey	key = top->key;

	ps_top_clear_bits(&key, hash, 0, top->base);
	hash_search_with_hash_value(PsTopForks, &key, hash, HASH_REMOVE, NULL);
}

/* Copy a top layer fork's bitmap below base out of shared memory. */
static uint32 *
ps_top_snapshot_bits(const PsForkKey *key, uint32 hash, BlockNumber base)
{
	size_t		nwords = ps_bitmap_words(base);
	uint32	   *bits;

	bits = MemoryContextAllocZero(PsCxt ? PsCxt : CurrentMemoryContext,
								  Max(nwords, 1) * sizeof(uint32));
	for (uint32 chunkno = 0;
		 (uint64) chunkno * PS_CHUNK_BLOCKS < base;
		 chunkno++)
	{
		PsChunk    *chunk = ps_chunk_lookup(key, hash, chunkno, false);
		size_t		first = (size_t) chunkno * PS_CHUNK_WORDS;

		if (chunk == NULL)
			continue;
		memcpy(bits + first, chunk->bits,
			   Min(PS_CHUNK_WORDS, nwords - first) * sizeof(uint32));
	}
	return bits;
}

/*
 * Load the top layer's files into shared memory, repairing what a crash can
 * leave half done.  Runs in the postmaster, or a standalone backend, while
 * setting up shared memory.
 */
static void
ps_load_top_layer(void)
{
	MemoryContext tmpcxt;
	MemoryContext oldcxt;
	HTAB	   *seen;
	HASHCTL		ctl;
	HASH_SEQ_STATUS status;
	struct
	{
		PsForkKey	key;
		bool		has_data;
		bool		has_map;
	}		   *ent;
	uint32		topgen = PsCtl->layers[PsCtl->nlayers - 1];
	char		dir[MAXPGPATH];
	DIR		   *dirdesc;
	struct dirent *de;

	tmpcxt = AllocSetContextCreate(CurrentMemoryContext, "page store load",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	ctl.keysize = sizeof(PsForkKey);
	ctl.entrysize = sizeof(*ent);
	ctl.hcxt = tmpcxt;
	seen = hash_create("page store load", 1024, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	ps_layer_path(dir, topgen);
	dirdesc = AllocateDir(dir);
	while ((de = ReadDir(dirdesc, dir)) != NULL)
	{
		PsForkKey	key;
		bool		is_map,
					is_tmp,
					found;
		char		path[MAXPGPATH];

		if (!ps_parse_name(de->d_name, &key, &is_map, &is_tmp))
			continue;
		if (is_tmp)
		{
			snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
			if (unlink(path) < 0)
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
			continue;
		}
		ent = hash_search(seen, &key, HASH_ENTER, &found);
		if (!found)
			ent->has_data = ent->has_map = false;
		if (is_map)
			ent->has_map = true;
		else
			ent->has_data = true;
	}
	FreeDir(dirdesc);

	hash_seq_init(&status, seen);
	while ((ent = hash_seq_search(&status)) != NULL)
	{
		uint32		hash = ps_fork_hash(&ent->key);
		PsTopFork  *top = ps_top_insert(&ent->key, hash);
		char		path[MAXPGPATH];
		uint32		flags = 0;
		BlockNumber base = 0;
		uint32	   *bits = NULL;

		ps_data_path(path, topgen, &ent->key);

		if (ent->has_map)
		{
			char		mpath[MAXPGPATH];

			ps_map_path(mpath, topgen, &ent->key);
			bits = ps_read_map(mpath, &flags, &base, tmpcxt, NULL);
			top->has_map = true;
		}

		if (flags & PS_MAP_DROPPED)
		{
			/* A crash left the data file of a fork dropped over lower layers */
			if (ent->has_data && unlink(path) < 0)
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
			top->kind = PS_FORK_DROPPED;
			continue;
		}

		top->kind = PS_FORK_DATA;
		top->base = base;
		if (base > 0 || !ent->has_data)
		{
			struct stat st;

			/*
			 * The data file is created after the map that gives it a base,
			 * and extended to base blocks then; a crash may have prevented
			 * either.
			 */
			if (!ent->has_data || stat(path, &st) < 0 ||
				st.st_size < (off_t) base * BLCKSZ)
			{
				int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);

				if (fd < 0 || ftruncate(fd, (off_t) base * BLCKSZ) < 0 ||
					pg_fsync(fd) != 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not extend file \"%s\": %m", path)));
				CloseTransientFile(fd);
			}

			for (BlockNumber blkno = 0; bits && blkno < base; blkno++)
			{
				if (bits[blkno / 32] & (1U << (blkno % 32)))
					ps_top_set_bit(&ent->key, hash, blkno);
			}
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);
}


/*
 * Frozen layers
 */

/* Read a frozen layer's directory into ps_frozen, if not done already. */
static void
ps_index_layer(uint32 gen)
{
	char		dir[MAXPGPATH];
	DIR		   *dirdesc;
	struct dirent *de;
	bool		found;

	if (ps_frozen == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(PsFrozenKey);
		ctl.entrysize = sizeof(PsFrozenFork);
		ctl.hcxt = PsCxt;
		ps_frozen = hash_create("page store frozen layers", 1024, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		ctl.keysize = sizeof(uint32);
		ctl.entrysize = sizeof(uint32);
		ps_indexed_gens = hash_create("page store indexed layers", 16, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	hash_search(ps_indexed_gens, &gen, HASH_FIND, &found);
	if (found)
		return;

	ps_layer_path(dir, gen);
	dirdesc = AllocateDir(dir);
	while ((de = ReadDir(dirdesc, dir)) != NULL)
	{
		PsFrozenKey fkey;
		PsFrozenFork *f;
		bool		is_map,
					is_tmp;

		memset(&fkey, 0, sizeof(fkey));
		if (!ps_parse_name(de->d_name, &fkey.fork, &is_map, &is_tmp) || is_tmp)
			continue;
		fkey.gen = gen;

		f = hash_search(ps_frozen, &fkey, HASH_ENTER, &found);
		if (!found)
		{
			f->has_data = f->has_map = f->loaded = f->absent = false;
			f->bits = NULL;
		}
		if (is_map)
			f->has_map = true;
		else
			f->has_data = true;
	}
	FreeDir(dirdesc);

	hash_search(ps_indexed_gens, &gen, HASH_ENTER, NULL);
}

static PsFrozenFork *
ps_frozen_lookup(uint32 gen, const PsForkKey *key)
{
	PsFrozenKey fkey;
	PsFrozenFork *f;

	ps_index_layer(gen);

	memset(&fkey, 0, sizeof(fkey));
	fkey.gen = gen;
	fkey.fork = *key;
	f = hash_search(ps_frozen, &fkey, HASH_FIND, NULL);
	if (f == NULL || f->loaded)
		return (f && f->absent) ? NULL : f;

	/*
	 * PageStoreReclaim() may have removed the files since the layer was read.
	 * Resolution never gets here for a fork whose files were reclaimed,
	 * unless the layer only held blocks the layers above hide, so the layer
	 * is then transparent for it.
	 */
	f->kind = PS_FORK_DATA;
	f->base = 0;
	f->bits = NULL;
	f->absent = false;
	if (f->has_map)
	{
		char		path[MAXPGPATH];
		uint32		flags = 0;
		bool		missing;

		ps_map_path(path, gen, key);
		f->bits = ps_read_map(path, &flags, &f->base, PsCxt, &missing);
		if (missing)
			f->has_map = false;
		if (flags & PS_MAP_DROPPED)
			f->kind = PS_FORK_DROPPED;
	}
	f->nblocks = f->base;
	if (f->kind == PS_FORK_DATA && f->has_data)
	{
		char		path[MAXPGPATH];
		struct stat st;

		ps_data_path(path, gen, key);
		if (stat(path, &st) < 0)
		{
			if (errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", path)));
			f->has_data = false;
		}
		else
			f->nblocks = Max(f->nblocks, (BlockNumber) (st.st_size / BLCKSZ));
	}
	f->loaded = true;
	if (!f->has_map && !f->has_data)
	{
		f->absent = true;
		return NULL;
	}
	return f;
}

static inline bool
ps_frozen_bit(const PsFrozenFork *f, BlockNumber blkno)
{
	return f->bits != NULL && (f->bits[blkno / 32] & (1U << (blkno % 32))) != 0;
}


/*
 * Resolution.  Callers hold the fork's partition lock and have synced the
 * stack; top is the fork's top layer entry, or NULL.
 */

/*
 * Return the generation of the layer whose data file holds the fork's size,
 * or 0 if the fork doesn't exist.  If that's a frozen layer, *frozen is set
 * to its entry.
 */
static uint32
ps_resolve_fork(const PsForkKey *key, PsTopFork *top, PsFrozenFork **frozen)
{
	*frozen = NULL;
	if (top)
		return top->kind == PS_FORK_DROPPED ? 0 : PS_TOPGEN();

	for (int i = ps_local_nlayers - 2; i >= 0; i--)
	{
		PsFrozenFork *f = ps_frozen_lookup(ps_local_layers[i], key);

		if (f == NULL)
			continue;
		if (f->kind == PS_FORK_DROPPED)
			return 0;
		*frozen = f;
		return ps_local_layers[i];
	}
	return 0;
}

/* Return the generation of the layer holding a block, or 0 if none does. */
static uint32
ps_resolve_block(const PsForkKey *key, uint32 hash, PsTopFork *top,
				 BlockNumber blkno)
{
	if (top)
	{
		if (top->kind == PS_FORK_DROPPED)
			return 0;
		if (blkno >= top->base || ps_top_bit(key, hash, blkno))
			return PS_TOPGEN();
	}

	for (int i = ps_local_nlayers - 2; i >= 0; i--)
	{
		PsFrozenFork *f = ps_frozen_lookup(ps_local_layers[i], key);

		if (f == NULL)
			continue;
		if (f->kind == PS_FORK_DROPPED)
			return 0;
		if (blkno >= f->base || ps_frozen_bit(f, blkno))
			return ps_local_layers[i];
	}
	return 0;
}

/*
 * Find the layer holding blocknum, and how many of the following blocks, up
 * to maxblocks in all, the same layer holds.  Returns that number, or 0 if
 * the fork doesn't exist.
 */
static int
ps_resolve_run(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int maxblocks, uint32 *gen)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	int			n;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_SHARED);
	ps_sync_stack();
	top = ps_top_lookup(&key, hash);
	*gen = ps_resolve_block(&key, hash, top, blocknum);
	if (*gen == 0)
	{
		LWLockRelease(lock);
		return 0;
	}
	for (n = 1; n < maxblocks && (uint64) blocknum + n < MaxBlockNumber; n++)
	{
		if (ps_resolve_block(&key, hash, top, blocknum + n) != *gen)
			break;
	}
	LWLockRelease(lock);
	return n;
}

static void
ps_report_missing(SMgrRelation reln, ForkNumber forknum)
{
	errno = ENOENT;
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not open file \"%s\": %m",
					relpath(reln->smgr_rlocator, forknum).str)));
}


/*
 * Open files
 */

static File
ps_open_file(SMgrRelation reln, ForkNumber forknum, uint32 gen, bool create)
{
	PsOpenFile *files = reln->ps_files[forknum];
	int			nfiles = reln->ps_num_files[forknum];
	char		path[MAXPGPATH];
	PsForkKey	key;
	File		vfd;

	for (int i = 0; i < nfiles; i++)
	{
		if (files[i].gen == gen)
			return files[i].vfd;
	}

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	ps_data_path(path, gen, &key);
	vfd = PathNameOpenFile(path, ps_open_flags() | (create ? O_CREAT | O_TRUNC : 0));
	if (vfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	if (nfiles == 0)
		files = MemoryContextAlloc(PsCxt, sizeof(PsOpenFile));
	else
		files = repalloc(files, sizeof(PsOpenFile) * (nfiles + 1));
	files[nfiles].gen = gen;
	files[nfiles].vfd = vfd;
	reln->ps_files[forknum] = files;
	reln->ps_num_files[forknum] = nfiles + 1;
	return vfd;
}

/*
 * Making the top layer writable for a fork
 */

/*
 * Return the fork's top layer entry, giving the fork files in the top layer
 * if it has none yet.  Errors out if the fork doesn't exist.  Caller holds
 * the freeze lock in shared mode, and the fork's partition lock exclusively.
 */
static PsTopFork *
ps_ensure_top(SMgrRelation reln, ForkNumber forknum, const PsForkKey *key,
			  uint32 hash)
{
	PsTopFork  *top;
	PsFrozenFork *lower;
	uint32		topgen = PS_TOPGEN();
	File		vfd;

	top = ps_top_lookup(key, hash);
	if (top)
	{
		if (top->kind == PS_FORK_DROPPED)
			ps_report_missing(reln, forknum);
		return top;
	}

	if (ps_resolve_fork(key, NULL, &lower) == 0)
		ps_report_missing(reln, forknum);

	/*
	 * The blocks below the lower layer's size stay there until written.  The
	 * map saying so must be durable before the data file exists, or a crash
	 * could leave a data file that hides them.
	 */
	ps_write_map(topgen, key, 0, lower->nblocks, NULL, true, ERROR);

	vfd = ps_open_file(reln, forknum, topgen, true);
	if (FileTruncate(vfd, (pgoff_t) lower->nblocks * BLCKSZ,
					 WAIT_EVENT_DATA_FILE_EXTEND) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend file \"%s\": %m",
						FilePathName(vfd))));

	top = ps_top_insert(key, hash);
	top->kind = PS_FORK_DATA;
	top->has_map = true;
	top->unsynced = true;
	top->base = lower->nblocks;
	return top;
}

static void ps_register_sync(const PsForkKey *key, uint32 gen);
static void ps_immedsync_layer(const PsForkKey *key, uint32 gen);


/*
 * The storage manager interface
 */

void
psinit(void)
{
	PsCxt = AllocSetContextCreate(TopMemoryContext,
								  "PageStoreSmgr",
								  ALLOCSET_DEFAULT_SIZES);

	/* pstruncate() runs in a critical section */
	MemoryContextAllowInCriticalSection(PsCxt, true);
}

void
psopen(SMgrRelation reln)
{
	for (int forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		reln->ps_num_files[forknum] = 0;
		reln->ps_files[forknum] = NULL;
	}
}

void
psclose(SMgrRelation reln, ForkNumber forknum)
{
	for (int i = 0; i < reln->ps_num_files[forknum]; i++)
		FileClose(reln->ps_files[forknum][i].vfd);
	if (reln->ps_files[forknum])
		pfree(reln->ps_files[forknum]);
	reln->ps_files[forknum] = NULL;
	reln->ps_num_files[forknum] = 0;
}

bool
psexists(SMgrRelation reln, ForkNumber forknum)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsFrozenFork *lower;
	uint32		gen;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_SHARED);
	ps_sync_stack();
	gen = ps_resolve_fork(&key, ps_top_lookup(&key, hash), &lower);
	LWLockRelease(lock);

	return gen != 0;
}

void
pscreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	PsFrozenFork *lower;
	uint32		topgen;

	/* See mdcreate() */
	TablespaceCreateDbspace(reln->smgr_rlocator.locator.spcOid,
							reln->smgr_rlocator.locator.dbOid,
							isRedo);

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(&PsCtl->freeze_lock, LW_SHARED);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	ps_sync_stack();
	topgen = PS_TOPGEN();

	top = ps_top_lookup(&key, hash);
	if (ps_resolve_fork(&key, top, &lower) != 0)
	{
		LWLockRelease(lock);
		LWLockRelease(&PsCtl->freeze_lock);
		if (isRedo)
			return;
		errno = EEXIST;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						relpath(reln->smgr_rlocator, forknum).str)));
	}

	/*
	 * The data file first: a crash after it leaves a dropped fork dropped, or
	 * an orphan file that redo of the creation reuses.
	 */
	(void) ps_open_file(reln, forknum, topgen, true);

	if (top)
	{
		/*
		 * The fork was dropped over lower layers.  Replace the map saying so
		 * by one saying the top layer owns all of the new, empty fork.
		 */
		Assert(top->kind == PS_FORK_DROPPED);
		ps_write_map(topgen, &key, 0, 0, NULL, true, ERROR);
	}
	else
		top = ps_top_insert(&key, hash);

	top->kind = PS_FORK_DATA;
	top->base = 0;
	top->unsynced = true;

	LWLockRelease(lock);

	ps_register_sync(&key, topgen);
	LWLockRelease(&PsCtl->freeze_lock);
}

/*
 * Unlink one fork.  See mdunlink() for why the main fork of a relation that
 * only the top layer has is left behind empty until the next checkpoint.
 * Failures are reported as WARNINGs, since we are usually past commit.
 */
static void
ps_unlink_fork(RelFileLocator rlocator, ForkNumber forknum, bool isRedo)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	PsFrozenFork *lower;
	uint32		topgen;
	bool		in_lower;
	bool		tombstone = false;
	char		path[MAXPGPATH];

	ps_key_init(&key, rlocator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(&PsCtl->freeze_lock, LW_SHARED);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	ps_sync_stack();
	topgen = PS_TOPGEN();
	ps_data_path(path, topgen, &key);

	top = ps_top_lookup(&key, hash);
	in_lower = ps_resolve_fork(&key, NULL, &lower) != 0;

	if (top && top->kind == PS_FORK_DROPPED)
	{
		/* already dropped */
	}
	else if (in_lower)
	{
		/*
		 * Hide the lower layers' blocks.  The map saying so must be durable
		 * before the data file goes, or a crash could expose them again.
		 */
		if (ps_write_map(topgen, &key, PS_MAP_DROPPED, 0, NULL, true, WARNING))
		{
			if (top)
				ps_top_clear_bits(&key, hash, 0, top->base);
			else
				top = ps_top_insert(&key, hash);
			top->kind = PS_FORK_DROPPED;
			top->base = 0;
			top->has_map = true;
			top->map_dirty = false;

			if (unlink(path) < 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}
	}
	else if (top)
	{
		/* Prevent other backends' fds from holding on to the disk space */
		if (pg_truncate(path, 0) < 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not truncate file \"%s\": %m", path)));

		if (top->has_map)
		{
			char		mpath[MAXPGPATH];

			ps_map_path(mpath, topgen, &key);
			if (unlink(mpath) < 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", mpath)));
		}

		if (isRedo || IsBinaryUpgrade || forknum != MAIN_FORKNUM)
		{
			if (unlink(path) < 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
			ps_top_remove(top, hash);
		}
		else
		{
			ps_top_clear_bits(&key, hash, 0, top->base);
			top->base = 0;
			top->has_map = false;
			top->map_dirty = false;
			tombstone = true;
		}
	}

	LWLockRelease(lock);
	LWLockRelease(&PsCtl->freeze_lock);

	if (tombstone)
	{
		FileTag		tag;

		INIT_PS_FILETAG(tag, rlocator, MAIN_FORKNUM, topgen);
		RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true /* retryOnError */ );
	}
}

void
psunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	if (forknum == InvalidForkNumber)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			ps_unlink_fork(rlocator.locator, forknum, isRedo);
	}
	else
		ps_unlink_fork(rlocator.locator, forknum, isRedo);
}

/*
 * Give the fork files in the top layer, and return the top layer's
 * generation, holding the freeze lock in shared mode.  The caller writes to
 * the top layer's data file, then releases the freeze lock.
 */
static uint32
ps_begin_write(SMgrRelation reln, ForkNumber forknum, BlockNumber *base)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	uint32		gen;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(&PsCtl->freeze_lock, LW_SHARED);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	ps_sync_stack();
	top = ps_ensure_top(reln, forknum, &key, hash);
	top->unsynced = true;
	gen = PS_TOPGEN();
	if (base)
		*base = top->base;
	LWLockRelease(lock);
	return gen;
}

/*
 * Note that a top layer fork's data file has a write an fsync hasn't
 * covered yet.  Called after the write: a sync that starts while the write
 * is in flight clears the mark, but needn't see the write.
 */
static void
ps_mark_unsynced(const PsForkKey *key)
{
	uint32		hash = ps_fork_hash(key);
	LWLock	   *lock = ps_partition_lock(hash);
	PsTopFork  *top;

	LWLockAcquire(lock, LW_EXCLUSIVE);
	top = ps_top_lookup(key, hash);
	if (top && top->kind == PS_FORK_DATA)
		top->unsynced = true;
	LWLockRelease(lock);
}

static void
ps_end_write(SMgrRelation reln, ForkNumber forknum, uint32 gen,
			 bool skipFsync)
{
	PsForkKey	key;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	ps_mark_unsynced(&key);
	if (!skipFsync)
		ps_register_sync(&key, gen);
	LWLockRelease(&PsCtl->freeze_lock);
}

void
psextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void *buffer, bool skipFsync)
{
	pgoff_t		seekpos;
	ssize_t		nbytes;
	uint32		gen;
	File		vfd;

	/* If this build supports direct I/O, the buffer must be I/O aligned. */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	if (blocknum == InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum).str,
						InvalidBlockNumber)));

	gen = ps_begin_write(reln, forknum, NULL);
	vfd = ps_open_file(reln, forknum, gen, false);
	seekpos = (pgoff_t) BLCKSZ * blocknum;

	if ((nbytes = FileWrite(vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_EXTEND)) != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m",
							FilePathName(vfd)),
					 errhint("Check free disk space.")));
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not extend file \"%s\": wrote only %zd of %zu bytes at block %u",
						FilePathName(vfd),
						nbytes, (size_t) BLCKSZ, blocknum),
				 errhint("Check free disk space.")));
	}

	ps_end_write(reln, forknum, gen, skipFsync);
}

void
pszeroextend(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber blocknum, int nblocks, bool skipFsync)
{
	pgoff_t		seekpos = (pgoff_t) BLCKSZ * blocknum;
	pgoff_t		amount = (pgoff_t) BLCKSZ * nblocks;
	uint32		gen;
	File		vfd;
	int			ret;

	Assert(nblocks > 0);

	if ((uint64) blocknum + nblocks >= (uint64) InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum).str,
						InvalidBlockNumber)));

	gen = ps_begin_write(reln, forknum, NULL);
	vfd = ps_open_file(reln, forknum, gen, false);

	/* See mdzeroextend() */
#ifdef HAVE_POSIX_FALLOCATE
	if (nblocks > 8 &&
		file_extend_method == FILE_EXTEND_METHOD_POSIX_FALLOCATE)
		ret = FileFallocate(vfd, seekpos, amount, WAIT_EVENT_DATA_FILE_EXTEND);
	else
#endif
		ret = FileZero(vfd, seekpos, amount, WAIT_EVENT_DATA_FILE_EXTEND) < 0 ? -1 : 0;
	if (ret != 0)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not extend file \"%s\": %m", FilePathName(vfd)),
				errhint("Check free disk space."));

	ps_end_write(reln, forknum, gen, skipFsync);
}

bool
psprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   int nblocks)
{
#ifdef USE_PREFETCH

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	if ((uint64) blocknum + nblocks > (uint64) MaxBlockNumber + 1)
		return false;

	while (nblocks > 0)
	{
		uint32		gen;
		int			run = ps_resolve_run(reln, forknum, blocknum, nblocks, &gen);
		File		vfd;

		if (run == 0)
		{
			if (InRecovery)
				return false;
			ps_report_missing(reln, forknum);
		}

		vfd = ps_open_file(reln, forknum, gen, false);
		(void) FilePrefetch(vfd, (pgoff_t) BLCKSZ * blocknum,
							(pgoff_t) BLCKSZ * run,
							WAIT_EVENT_DATA_FILE_PREFETCH);

		blocknum += run;
		nblocks -= run;
	}
#endif							/* USE_PREFETCH */

	return true;
}

/* See md.c */
static int
buffers_to_iovec(struct iovec *iov, void **buffers, int nblocks)
{
	struct iovec *iovp;
	int			iovcnt;

	Assert(nblocks >= 1);

	for (int i = 0; i < nblocks; ++i)
	{
		if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
			Assert((uintptr_t) buffers[i] ==
				   TYPEALIGN(PG_IO_ALIGN_SIZE, buffers[i]));
	}

	iovp = &iov[0];
	iovp->iov_base = buffers[0];
	iovp->iov_len = BLCKSZ;
	iovcnt = 1;

	for (int i = 1; i < nblocks; ++i)
	{
		void	   *buffer = buffers[i];

		if (((char *) iovp->iov_base + iovp->iov_len) == buffer)
			iovp->iov_len += BLCKSZ;
		else
		{
			iovp++;
			iovp->iov_base = buffer;
			iovp->iov_len = BLCKSZ;
			iovcnt++;
		}
	}

	return iovcnt;
}

/*
 * A read can be combined across the blocks one layer holds.
 */
uint32
psmaxcombine(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	uint32		gen;
	int			run;

	run = ps_resolve_run(reln, forknum, blocknum, MAX_IO_COMBINE_LIMIT, &gen);
	return Max(run, 1);
}

void
psreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		void **buffers, BlockNumber nblocks)
{
	while (nblocks > 0)
	{
		struct iovec iov[PG_IOV_MAX];
		int			iovcnt;
		pgoff_t		seekpos;
		ssize_t		nbytes;
		uint32		gen;
		int			run;
		File		vfd;
		size_t		transferred = 0;
		size_t		size;

		run = ps_resolve_run(reln, forknum, blocknum,
							 (int) Min(nblocks, lengthof(iov)), &gen);
		if (run == 0)
			ps_report_missing(reln, forknum);

		vfd = ps_open_file(reln, forknum, gen, false);
		seekpos = (pgoff_t) BLCKSZ * blocknum;
		iovcnt = buffers_to_iovec(iov, buffers, run);
		size = (size_t) run * BLCKSZ;

		for (;;)
		{
			nbytes = FileReadV(vfd, iov, iovcnt, seekpos,
							   WAIT_EVENT_DATA_FILE_READ);
			if (nbytes < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read blocks %u..%u in file \"%s\": %m",
								blocknum, blocknum + run - 1,
								FilePathName(vfd))));
			if (nbytes == 0)
			{
				/* See mdreadv() */
				if (zero_damaged_pages || InRecovery)
				{
					for (int i = transferred / BLCKSZ; i < run; ++i)
						memset(buffers[i], 0, BLCKSZ);
					break;
				}
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not read blocks %u..%u in file \"%s\": read only %zu of %zu bytes",
								blocknum, blocknum + run - 1,
								FilePathName(vfd), transferred, size)));
			}

			transferred += nbytes;
			if (transferred == size)
				break;
			seekpos += nbytes;
			iovcnt = compute_remaining_iovec(iov, iov, iovcnt, nbytes);
		}

		nblocks -= run;
		buffers += run;
		blocknum += run;
	}
}

void
psstartreadv(PgAioHandle *ioh,
			 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 void **buffers, BlockNumber nblocks)
{
	struct iovec *iov;
	int			iovcnt;
	uint32		gen;
	int			run;
	File		vfd;
	int			ret;

	run = ps_resolve_run(reln, forknum, blocknum, (int) nblocks, &gen);
	if (run == 0)
		ps_report_missing(reln, forknum);
	if ((BlockNumber) run != nblocks)
		elog(ERROR, "read crosses page store layer boundary");

	vfd = ps_open_file(reln, forknum, gen, false);

	iovcnt = pgaio_io_get_iovec(ioh, &iov);
	Assert(nblocks <= iovcnt);
	iovcnt = buffers_to_iovec(iov, buffers, nblocks);

	if (!(io_direct_flags & IO_DIRECT_DATA))
		pgaio_io_set_flag(ioh, PGAIO_HF_BUFFERED);

	pgaio_io_set_target_smgr(ioh, reln, forknum, blocknum, nblocks, false);

	/* md.c's completion callbacks know nothing of md.c's file layout */
	pgaio_io_register_callbacks(ioh, PGAIO_HCB_MD_READV, 0);

	ret = FileStartReadV(ioh, vfd, iovcnt, (pgoff_t) BLCKSZ * blocknum,
						 WAIT_EVENT_DATA_FILE_READ);
	if (ret != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not start reading blocks %u..%u in file \"%s\": %m",
						blocknum, blocknum + nblocks - 1,
						FilePathName(vfd))));
}

void
pswritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	BlockNumber base;
	uint32		gen;
	File		vfd;
	BlockNumber start = blocknum;
	BlockNumber total = nblocks;

	gen = ps_begin_write(reln, forknum, &base);
	vfd = ps_open_file(reln, forknum, gen, false);

	while (nblocks > 0)
	{
		struct iovec iov[PG_IOV_MAX];
		int			iovcnt;
		pgoff_t		seekpos = (pgoff_t) BLCKSZ * blocknum;
		BlockNumber n = Min(nblocks, lengthof(iov));
		size_t		size = (size_t) n * BLCKSZ;
		size_t		transferred = 0;
		ssize_t		nbytes;

		iovcnt = buffers_to_iovec(iov, unconstify(void **, buffers), n);
		for (;;)
		{
			nbytes = FileWriteV(vfd, iov, iovcnt, seekpos,
								WAIT_EVENT_DATA_FILE_WRITE);
			if (nbytes < 0)
			{
				bool		enospc = errno == ENOSPC;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write blocks %u..%u in file \"%s\": %m",
								blocknum, blocknum + n - 1,
								FilePathName(vfd)),
						 enospc ? errhint("Check free disk space.") : 0));
			}
			transferred += nbytes;
			if (transferred == size)
				break;
			seekpos += nbytes;
			iovcnt = compute_remaining_iovec(iov, iov, iovcnt, nbytes);
		}

		nblocks -= n;
		buffers += n;
		blocknum += n;
	}

	/* The top layer now holds the blocks it inherited until now */
	if (start < base)
	{
		PsForkKey	key;
		uint32		hash;
		LWLock	   *lock;
		PsTopFork  *top;

		ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
		hash = ps_fork_hash(&key);
		lock = ps_partition_lock(hash);

		LWLockAcquire(lock, LW_EXCLUSIVE);
		top = ps_top_lookup(&key, hash);
		if (top && top->kind == PS_FORK_DATA)
		{
			for (BlockNumber b = start; b < start + total && b < top->base; b++)
				ps_top_set_bit(&key, hash, b);
			top->map_dirty = true;
			top->map_version++;
		}
		LWLockRelease(lock);
	}

	ps_end_write(reln, forknum, gen, skipFsync);
}

void
pswriteback(SMgrRelation reln, ForkNumber forknum,
			BlockNumber blocknum, BlockNumber nblocks)
{
	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	/*
	 * Only the top layer is written to, and we only flush a file we have open
	 * already, as md.c does.
	 */
	for (int i = 0; i < reln->ps_num_files[forknum]; i++)
	{
		if (ps_local_nlayers > 0 &&
			reln->ps_files[forknum][i].gen == PS_TOPGEN())
			FileWriteback(reln->ps_files[forknum][i].vfd,
						  (pgoff_t) BLCKSZ * blocknum,
						  (pgoff_t) BLCKSZ * nblocks,
						  WAIT_EVENT_DATA_FILE_FLUSH);
	}
}

BlockNumber
psnblocks(SMgrRelation reln, ForkNumber forknum)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsFrozenFork *lower;
	uint32		gen;
	File		vfd;
	pgoff_t		len;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_SHARED);
	ps_sync_stack();
	gen = ps_resolve_fork(&key, ps_top_lookup(&key, hash), &lower);
	LWLockRelease(lock);

	if (gen == 0)
		ps_report_missing(reln, forknum);
	if (lower)
		return lower->nblocks;

	vfd = ps_open_file(reln, forknum, gen, false);
	len = FileSize(vfd);
	if (len < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m",
						FilePathName(vfd))));
	return (BlockNumber) (len / BLCKSZ);
}

/*
 * Truncate a fork.  Like mdtruncate(), this runs in a critical section.
 */
void
pstruncate(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber curnblk, BlockNumber nblocks)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	uint32		gen;
	bool		base_shrank = false;
	File		vfd;

	if (nblocks > curnblk)
	{
		/* Bogus request ... but no complaint if InRecovery */
		if (InRecovery)
			return;
		ereport(ERROR,
				(errmsg("could not truncate file \"%s\" to %u blocks: it's only %u blocks now",
						relpath(reln->smgr_rlocator, forknum).str,
						nblocks, curnblk)));
	}
	if (nblocks == curnblk)
		return;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(&PsCtl->freeze_lock, LW_SHARED);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	ps_sync_stack();
	top = ps_ensure_top(reln, forknum, &key, hash);
	top->unsynced = true;
	gen = PS_TOPGEN();
	if (nblocks < top->base)
	{
		ps_top_clear_bits(&key, hash, nblocks, top->base);
		top->base = nblocks;
		top->map_dirty = true;
		top->map_version++;
		base_shrank = true;
	}
	LWLockRelease(lock);

	/*
	 * If the fork grew again past the new end, the blocks there would be read
	 * from the lower layers unless the smaller base was durable before the
	 * data file shrinks.
	 */
	if (base_shrank)
		ps_immedsync_layer(&key, gen);

	vfd = ps_open_file(reln, forknum, gen, false);
	if (FileTruncate(vfd, (pgoff_t) nblocks * BLCKSZ, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\" to %u blocks: %m",
						FilePathName(vfd), nblocks)));

	ps_mark_unsynced(&key);
	ps_register_sync(&key, gen);
	LWLockRelease(&PsCtl->freeze_lock);
}

/* Call fn for each layer that has files for the fork. */
static void
ps_foreach_layer(SMgrRelation reln, ForkNumber forknum,
				 void (*fn) (const PsForkKey *key, uint32 gen))
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	uint32		layers[PAGESTORE_MAX_LAYERS];
	int			nlayers = 0;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_SHARED);
	ps_sync_stack();
	if (ps_top_lookup(&key, hash))
		layers[nlayers++] = PS_TOPGEN();
	for (int i = ps_local_nlayers - 2; i >= 0; i--)
	{
		if (ps_frozen_lookup(ps_local_layers[i], &key))
			layers[nlayers++] = ps_local_layers[i];
	}
	LWLockRelease(lock);

	for (int i = 0; i < nlayers; i++)
		fn(&key, layers[i]);
}

static void
ps_immedsync_layer(const PsForkKey *key, uint32 gen)
{
	FileTag		tag;
	char		path[MAXPGPATH];

	INIT_PS_FILETAG(tag, key->rlocator, key->forknum, gen);
	if (pssyncfiletag(&tag, path) < 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
}

/*
 * Writes skipping fsync may have gone to a layer that has been frozen since,
 * so these sync every layer that has the fork.
 */
void
psimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	ps_foreach_layer(reln, forknum, ps_immedsync_layer);
}

void
psregistersync(SMgrRelation reln, ForkNumber forknum)
{
	ps_foreach_layer(reln, forknum, ps_register_sync);
}

int
psfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint64 *off)
{
	uint32		gen;

	if (ps_resolve_run(reln, forknum, blocknum, 1, &gen) == 0)
		ps_report_missing(reln, forknum);

	*off = (uint64) BLCKSZ * blocknum;
	return FileGetRawDesc(ps_open_file(reln, forknum, gen, false));
}


/*
 * Sync requests
 */

/*
 * Ask the checkpointer to sync one layer's files for a fork, or do it now if
 * the request queue is full.
 */
static void
ps_register_sync(const PsForkKey *key, uint32 gen)
{
	FileTag		tag;

	INIT_PS_FILETAG(tag, key->rlocator, key->forknum, gen);

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError */ ))
	{
		char		path[MAXPGPATH];

		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));

		if (pssyncfiletag(&tag, path) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", path)));
	}
}

/* fsync a file by path; a missing file is fine, it was dropped. */
static int
ps_fsync_path(const char *path)
{
	int			fd;
	int			save_errno;

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	if (pg_fsync(fd) != 0)
	{
		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
		return -1;
	}
	return CloseTransientFile(fd);
}

/*
 * Sync one layer's files for a fork.  For the top layer that means writing
 * its map: with the bits set before the data file was fsync'd, so that no
 * bit becomes durable before the block it describes.
 */
int
pssyncfiletag(const FileTag *ftag, char *path)
{
	PsForkKey	key;
	uint32		gen = (uint32) ftag->segno;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	uint32	   *bits = NULL;
	uint32		version = 0;
	bool		is_top;
	instr_time	io_start;

	ps_key_init(&key, ftag->rlocator, ftag->forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_EXCLUSIVE);
	ps_sync_stack();
	is_top = gen == PS_TOPGEN();
	if (is_top)
	{
		top = ps_top_lookup(&key, hash);
		if (top && top->kind == PS_FORK_DATA)
		{
			/* writes from here on are not covered by this fsync */
			top->unsynced = false;
			if (top->map_dirty)
			{
				version = top->map_version;
				bits = ps_top_snapshot_bits(&key, hash, top->base);
			}
		}
	}
	LWLockRelease(lock);

	io_start = pgstat_prepare_io_time(track_io_timing);

	ps_data_path(path, gen, &key);
	if (ps_fsync_path(path) < 0)
	{
		int			save_errno = errno;

		if (is_top)
		{
			LWLockAcquire(lock, LW_EXCLUSIVE);
			top = ps_top_lookup(&key, hash);
			if (top && top->kind == PS_FORK_DATA)
				top->unsynced = true;
			LWLockRelease(lock);
		}
		if (bits)
			pfree(bits);
		errno = save_errno;
		return -1;
	}

	if (bits)
	{
		LWLockAcquire(lock, LW_EXCLUSIVE);
		ps_sync_stack();
		top = ps_top_lookup(&key, hash);
		if (gen == PS_TOPGEN() && top && top->kind == PS_FORK_DATA)
		{
			/* bits above a base that shrank meanwhile are ignored */
			ps_write_map(gen, &key, 0, top->base, bits, true, ERROR);
			top->has_map = true;
			if (top->map_version == version)
				top->map_dirty = false;
			bits = NULL;
		}
		LWLockRelease(lock);
	}

	/*
	 * A layer that isn't the top (anymore) got its map from the freeze, which
	 * didn't make it durable.
	 */
	if (!is_top || bits)
	{
		char		dir[MAXPGPATH];

		ps_map_path(path, gen, &key);
		if (ps_fsync_path(path) < 0)
			return -1;

		/*
		 * The layer's directory entries were made while it was the top.  The
		 * directory is gone if PageStoreReclaim() emptied the layer.
		 */
		ps_layer_path(dir, gen);
		strlcpy(path, dir, MAXPGPATH);
		if (fsync_fname_ext(dir, true, false, LOG) != 0)
		{
			struct stat st;

			if (stat(dir, &st) < 0 && errno == ENOENT)
				return 0;
			return -1;
		}
	}
	if (bits)
		pfree(bits);

	pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
							IOOP_FSYNC, io_start, 1, 0);
	return 0;
}

/*
 * Write the top layer's maps that are out of date, when fsync is off.
 *
 * Normally pssyncfiletag() writes a fork's map when a checkpoint syncs the
 * fork, after fsyncing its data file.  With fsync off, ProcessSyncRequests()
 * doesn't call it, but the maps must still be written: a restart, even a
 * clean one, reads the top layer's bitmap from them.  Without fsync there is
 * no durability to order, so they're written here as they are.
 */
void
PageStoreCheckPoint(void)
{
	HASH_SEQ_STATUS status;
	PsTopFork  *top;

	if (!PageStoreEnabled() || enableFsync)
		return;

	ps_acquire_all_partitions(LW_EXCLUSIVE);
	ps_sync_stack();
	hash_seq_init(&status, PsTopForks);
	while ((top = hash_seq_search(&status)) != NULL)
	{
		uint32		hash;
		uint32	   *bits;

		if (top->kind != PS_FORK_DATA || !top->map_dirty)
			continue;
		hash = ps_fork_hash(&top->key);
		bits = ps_top_snapshot_bits(&top->key, hash, top->base);
		ps_write_map(PS_TOPGEN(), &top->key, 0, top->base, bits, false, ERROR);
		pfree(bits);
		top->has_map = true;
		top->map_dirty = false;
	}
	ps_release_all_partitions();
}

/*
 * Remove the empty data file a dropped relation's main fork left behind,
 * unless the top layer has been frozen since.  Then it's part of a layer
 * that may be shared, and stays.
 */
int
psunlinkfiletag(const FileTag *ftag, char *path)
{
	PsForkKey	key;
	uint32		gen = (uint32) ftag->segno;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	int			ret = 0;

	Assert(ftag->forknum == MAIN_FORKNUM);

	ps_key_init(&key, ftag->rlocator, MAIN_FORKNUM);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);
	ps_data_path(path, gen, &key);

	LWLockAcquire(lock, LW_EXCLUSIVE);
	ps_sync_stack();
	top = ps_top_lookup(&key, hash);
	if (gen == PS_TOPGEN() && top && top->kind == PS_FORK_DATA &&
		top->base == 0 && !top->has_map)
	{
		struct stat st;

		if (stat(path, &st) == 0 && st.st_size == 0)
		{
			ret = unlink(path);
			if (ret == 0)
				ps_top_remove(top, hash);
		}
	}
	LWLockRelease(lock);

	return ret;
}

bool
psfiletagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/* As in mdfiletagmatches(), filter requests drop a database's requests */
	return ftag->rlocator.dbOid == candidate->rlocator.dbOid;
}

void
PageStoreForgetDatabaseSyncRequests(Oid dbid)
{
	FileTag		tag;
	RelFileLocator rlocator;

	if (!PageStoreEnabled())
		return;

	rlocator.dbOid = dbid;
	rlocator.spcOid = 0;
	rlocator.relNumber = 0;

	INIT_PS_FILETAG(tag, rlocator, InvalidForkNumber, 0);

	RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true /* retryOnError */ );
}


/*
 * Operations on many relations
 */

/*
 * Return the forks that have files in any layer, of one database or of all
 * (dbid InvalidOid with all_dbs).  Some may not exist; callers check.
 */
static PsForkKey *
ps_list_forks(bool all_dbs, Oid dbid, int *nforks)
{
	HTAB	   *forks;
	HASHCTL		ctl;
	HASH_SEQ_STATUS status;
	PsTopFork  *top;
	PsFrozenFork *f;
	PsForkKey  *key;
	PsForkKey  *result;
	int			n = 0;

	ctl.keysize = sizeof(PsForkKey);
	ctl.entrysize = sizeof(PsForkKey);
	ctl.hcxt = CurrentMemoryContext;
	forks = hash_create("page store forks", 1024, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	ps_acquire_all_partitions(LW_SHARED);
	ps_sync_stack();
	hash_seq_init(&status, PsTopForks);
	while ((top = hash_seq_search(&status)) != NULL)
	{
		if (all_dbs || top->key.rlocator.dbOid == dbid)
			hash_search(forks, &top->key, HASH_ENTER, NULL);
	}
	for (int i = 0; i < ps_local_nlayers - 1; i++)
		ps_index_layer(ps_local_layers[i]);
	ps_release_all_partitions();

	if (ps_frozen)
	{
		hash_seq_init(&status, ps_frozen);
		while ((f = hash_seq_search(&status)) != NULL)
		{
			if (all_dbs || f->key.fork.rlocator.dbOid == dbid)
				hash_search(forks, &f->key.fork, HASH_ENTER, NULL);
		}
	}

	result = palloc_array(PsForkKey, Max(hash_get_num_entries(forks), 1));
	hash_seq_init(&status, forks);
	while ((key = hash_seq_search(&status)) != NULL)
		result[n++] = *key;
	hash_destroy(forks);

	*nforks = n;
	return result;
}

/*
 * Remove all of a database's relations, for DROP DATABASE.  Its buffers are
 * gone, its sync requests forgotten and its files closed already.
 */
void
PageStoreDropDatabase(Oid dbid)
{
	PsForkKey  *forks;
	int			nforks;

	if (!PageStoreEnabled())
		return;

	forks = ps_list_forks(false, dbid, &nforks);
	for (int i = 0; i < nforks; i++)
		ps_unlink_fork(forks[i].rlocator, forks[i].forknum, true);
	pfree(forks);
}

/* Copy one fork through the storage manager. */
static void
ps_copy_fork(SMgrRelation src, SMgrRelation dst, ForkNumber forknum,
			 bool isRedo)
{
	PGIOAlignedBlock buf;
	BlockNumber nblocks = smgrnblocks(src, forknum);

	smgrcreate(dst, forknum, isRedo);
	for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
	{
		CHECK_FOR_INTERRUPTS();
		smgrread(src, forknum, blkno, buf.data);
		smgrextend(dst, forknum, blkno, buf.data, true);
	}
	smgrimmedsync(dst, forknum);
}

/*
 * Copy a database's relations in pg_default to another database, in the
 * given tablespace, for CREATE DATABASE ... STRATEGY FILE_COPY.  The
 * source's buffers have been flushed.  The copy goes through the storage
 * manager, so it lands in md.c's files if the tablespace isn't pg_default.
 */
void
PageStoreCopyDatabase(Oid src_dbid, Oid dst_dbid, Oid dst_spcoid, bool isRedo)
{
	PsForkKey  *forks;
	int			nforks;

	if (!PageStoreEnabled())
		return;

	forks = ps_list_forks(false, src_dbid, &nforks);
	for (int i = 0; i < nforks; i++)
	{
		RelFileLocator dst_rlocator = forks[i].rlocator;
		SMgrRelation src;

		if (forks[i].rlocator.spcOid != DEFAULTTABLESPACE_OID)
			continue;
		src = smgropen(forks[i].rlocator, INVALID_PROC_NUMBER);
		if (!smgrexists(src, forks[i].forknum))
			continue;

		dst_rlocator.spcOid = dst_spcoid;
		dst_rlocator.dbOid = dst_dbid;
		ps_copy_fork(src, smgropen(dst_rlocator, INVALID_PROC_NUMBER),
					 forks[i].forknum, isRedo);
	}
	pfree(forks);
}

/*
 * Reset unlogged relations to their init forks, as ResetUnloggedRelations()
 * does for md.c's files.
 */
void
PageStoreResetUnloggedRelations(int op)
{
	PsForkKey  *forks;
	int			nforks;

	if (!PageStoreEnabled())
		return;

	forks = ps_list_forks(true, InvalidOid, &nforks);
	for (int i = 0; i < nforks; i++)
	{
		SMgrRelation reln;

		if (forks[i].forknum != INIT_FORKNUM)
			continue;
		reln = smgropen(forks[i].rlocator, INVALID_PROC_NUMBER);
		if (!smgrexists(reln, INIT_FORKNUM))
			continue;

		if (op & UNLOGGED_RELATION_CLEANUP)
		{
			for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			{
				if (forknum != INIT_FORKNUM && smgrexists(reln, forknum))
				{
					smgrclose(reln);
					ps_unlink_fork(forks[i].rlocator, forknum, true);
				}
			}
		}

		if (op & UNLOGGED_RELATION_INIT)
		{
			PGIOAlignedBlock buf;
			BlockNumber nblocks = smgrnblocks(reln, INIT_FORKNUM);

			if (smgrexists(reln, MAIN_FORKNUM))
			{
				smgrclose(reln);
				ps_unlink_fork(forks[i].rlocator, MAIN_FORKNUM, true);
			}
			smgrcreate(reln, MAIN_FORKNUM, true);
			for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
			{
				smgrread(reln, INIT_FORKNUM, blkno, buf.data);
				smgrextend(reln, MAIN_FORKNUM, blkno, buf.data, true);
			}
			smgrimmedsync(reln, MAIN_FORKNUM);
		}
		smgrclose(reln);
	}
	pfree(forks);
}

/*
 * The size of a fork, or 0 if it doesn't exist.  Like md.c's sizes, this
 * doesn't fail if the fork is dropped meanwhile.
 */
int64
PageStoreForkSize(RelFileLocator rlocator, ForkNumber forknum)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsFrozenFork *lower;
	uint32		gen;
	char		path[MAXPGPATH];
	struct stat st;

	ps_key_init(&key, rlocator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_SHARED);
	ps_sync_stack();
	gen = ps_resolve_fork(&key, ps_top_lookup(&key, hash), &lower);
	LWLockRelease(lock);

	if (gen == 0)
		return 0;
	if (lower)
		return (int64) lower->nblocks * BLCKSZ;

	ps_data_path(path, gen, &key);
	if (stat(path, &st) < 0)
	{
		if (errno == ENOENT)
			return 0;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}
	return (int64) (st.st_size / BLCKSZ) * BLCKSZ;
}

/* The size of all of a database's relations in the page store. */
int64
PageStoreDatabaseSize(Oid dbid)
{
	PsForkKey  *forks;
	int			nforks;
	int64		size = 0;

	if (!PageStoreEnabled())
		return 0;

	forks = ps_list_forks(false, dbid, &nforks);
	for (int i = 0; i < nforks; i++)
	{
		CHECK_FOR_INTERRUPTS();
		size += PageStoreForkSize(forks[i].rlocator, forks[i].forknum);
	}
	pfree(forks);
	return size;
}

/*
 * Is a relation file number taken?  Like GetNewRelFileNumber()'s check for
 * an existing file, this counts any trace of the relation in any layer,
 * including what dropping it left behind.
 */
bool
PageStoreRelFileNumberInUse(RelFileLocator rlocator)
{
	bool		in_use = false;

	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM && !in_use; forknum++)
	{
		PsForkKey	key;
		uint32		hash;
		LWLock	   *lock;

		ps_key_init(&key, rlocator, forknum);
		hash = ps_fork_hash(&key);
		lock = ps_partition_lock(hash);

		LWLockAcquire(lock, LW_SHARED);
		ps_sync_stack();
		in_use = ps_top_lookup(&key, hash) != NULL;
		for (int i = ps_local_nlayers - 2; i >= 0 && !in_use; i--)
			in_use = ps_frozen_lookup(ps_local_layers[i], &key) != NULL;
		LWLockRelease(lock);
	}
	return in_use;
}


/*
 * Branching
 */

/*
 * Freeze the top layer and start a new one, unless nothing was written to
 * the top layer since the last freeze.  Fills layers with the frozen layers'
 * generations, bottom first, and returns how many there are.  Their files
 * never change after this, so they can be linked into another data
 * directory.
 *
 * The files of the layer frozen are fsync'd before returning, if they have
 * been written since they last were, so that every frozen layer is durable.
 * A branch's first checkpoint counts on that.
 *
 * *insert_end is set to the end of WAL inserted while no write to the page
 * store could happen.  WAL up to there describes no write the frozen layers
 * lack: a record logged after its data was written, like the commit of a
 * relation created without WAL, falls after it if the write missed the
 * freeze.
 */
int
PageStoreFreeze(uint32 *layers, XLogRecPtr *insert_end)
{
	HASH_SEQ_STATUS status;
	PsTopFork  *top;
	PsChunk    *chunk;
	PsForkKey  *synckeys;
	int			nsync = 0;
	uint32		oldtop;
	uint32		newtop;
	uint32		newlayers[PAGESTORE_MAX_LAYERS];
	int			nfrozen;
	char		dir[MAXPGPATH];

	if (!PageStoreEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the cluster does not use the page store")));

	synckeys = palloc_array(PsForkKey, pagestore_max_forks);

	/* wait for writes in progress, and stop new ones */
	LWLockAcquire(&PsCtl->freeze_lock, LW_EXCLUSIVE);
	ps_acquire_all_partitions(LW_EXCLUSIVE);
	ps_sync_stack();
	oldtop = PS_TOPGEN();

	*insert_end = GetXLogInsertEndRecPtr();

	if (hash_get_num_entries(PsTopForks) == 0)
	{
		nfrozen = PsCtl->nlayers - 1;
		memcpy(layers, PsCtl->layers, sizeof(uint32) * nfrozen);
		ps_release_all_partitions();
		LWLockRelease(&PsCtl->freeze_lock);
		pfree(synckeys);
		return nfrozen;
	}

	if (PsCtl->nlayers >= PAGESTORE_MAX_LAYERS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the page store has too many layers")));

	/*
	 * Bring the maps up to date, for whoever reads the layer once it's
	 * frozen.  They don't need to be durable for that; the checkpointer makes
	 * them durable for us.
	 */
	hash_seq_init(&status, PsTopForks);
	while ((top = hash_seq_search(&status)) != NULL)
	{
		bool		map_written = false;

		if (top->kind == PS_FORK_DATA && top->map_dirty)
		{
			uint32		hash = ps_fork_hash(&top->key);
			uint32	   *bits = ps_top_snapshot_bits(&top->key, hash, top->base);

			ps_write_map(oldtop, &top->key, 0, top->base, bits, false, ERROR);
			pfree(bits);
			map_written = true;
		}
		if (top->unsynced || map_written)
			synckeys[nsync++] = top->key;
	}

	/* start the new top layer, in a directory a failed freeze may have left */
	newtop = oldtop + 1;
	ps_layer_path(dir, newtop);
	{
		struct stat st;

		if (stat(dir, &st) == 0 && !rmtree(dir, true))
			ereport(ERROR,
					(errmsg("could not remove directory \"%s\"", dir)));
	}
	if (MakePGDirectory(dir) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir)));
	fsync_fname(dir, true);

	memcpy(newlayers, PsCtl->layers, sizeof(uint32) * PsCtl->nlayers);
	newlayers[PsCtl->nlayers] = newtop;
	PageStoreWriteStackFile(NULL, newlayers, PsCtl->nlayers + 1);

	/* nothing below can fail */
	hash_seq_init(&status, PsChunks);
	while ((chunk = hash_seq_search(&status)) != NULL)
		hash_search_with_hash_value(PsChunks, &chunk->key,
									ps_chunk_hash(ps_fork_hash(&chunk->key.fork),
												  chunk->key.chunkno),
									HASH_REMOVE, NULL);
	hash_seq_init(&status, PsTopForks);
	while ((top = hash_seq_search(&status)) != NULL)
		hash_search_with_hash_value(PsTopForks, &top->key,
									ps_fork_hash(&top->key),
									HASH_REMOVE, NULL);

	PsCtl->layers[PsCtl->nlayers++] = newtop;
	PsCtl->stack_version++;

	nfrozen = PsCtl->nlayers - 1;
	memcpy(layers, PsCtl->layers, sizeof(uint32) * nfrozen);

	ps_release_all_partitions();
	LWLockRelease(&PsCtl->freeze_lock);

	for (int i = 0; i < nsync; i++)
	{
		char		path[MAXPGPATH];

		ps_data_path(path, oldtop, &synckeys[i]);
		if (ps_fsync_path(path) == 0)
		{
			ps_map_path(path, oldtop, &synckeys[i]);
			if (ps_fsync_path(path) == 0)
				continue;
		}
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	}
	if (nsync > 0)
	{
		ps_layer_path(dir, oldtop);
		fsync_fname(dir, true);
	}
	pfree(synckeys);

	return nfrozen;
}

/* Serializes pg_branch() calls */
LWLock *
PageStoreBranchLock(void)
{
	return &PsCtl->branch_lock;
}

void
PageStoreSetBranchKeepLSN(XLogRecPtr lsn)
{
	pg_atomic_write_membarrier_u64(&PsCtl->branch_keep_lsn, lsn);
}

/* Called by checkpoints, to keep the WAL a branch in progress copies. */
XLogRecPtr
PageStoreGetBranchKeepLSN(void)
{
	if (PsCtl == NULL)
		return InvalidXLogRecPtr;
	return pg_atomic_read_membarrier_u64(&PsCtl->branch_keep_lsn);
}


/*
 * Reclaiming space
 *
 * A frozen layer never changes, but how much of it this cluster can still
 * read shrinks as layers are frozen above it: a block that the layers above
 * hold hides the layer's copy, and a fork that the layers above dropped or
 * recreated hides all of the layer's files for it.  PageStoreReclaim() gives
 * that space back without copying any data:
 *
 *	-- a fork's files in a layer that holds none of the fork's blocks this
 *	   cluster can still read are unlinked.  A data directory sharing them
 *	   keeps its own link, or its own clone.
 *	-- hidden blocks of a data file that has no other link are punched out.
 *	   A clone, as pg_branch() makes on APFS, has its own copy of the blocks,
 *	   so punching them leaves it alone.  A hard-linked file may hold another
 *	   data directory's live blocks, so it is left alone until that
 *	   directory is gone.
 *	-- blocks a layer never wrote are punched out of its data file, linked
 *	   or not: every data directory sharing the file reads it through the
 *	   same map, which sends those blocks to the layers below.  They are
 *	   holes to begin with, except that APFS fills in the holes of a sparse
 *	   file around writes when it is fsync'd.
 *	-- a layer left without files is taken off the stack.
 *
 * Only frozen layers count as hiding blocks: their files are durable (see
 * PageStoreFreeze()), while the top layer's bitmap may lose bits in a crash.
 * Layers frozen while this runs only hide more, so working from the stack as
 * it was at the start is safe.  Reading a layer's metadata after its files
 * were reclaimed finds the layer transparent for the fork, which is right:
 * resolution only gets to the layer for blocks it doesn't hold.
 */

typedef struct PsDeadFork
{
	uint32		gen;
	PsForkKey	key;
} PsDeadFork;

static inline bool
ps_bitmap_test(const uint32 *bits, size_t nwords, uint64 blkno)
{
	return blkno / 32 < nwords && (bits[blkno / 32] & (1U << (blkno % 32))) != 0;
}

/* Does a frozen layer hold this block of the fork? */
static inline bool
ps_frozen_holds(const PsFrozenFork *f, uint64 blkno)
{
	return blkno >= f->base || ps_frozen_bit(f, (BlockNumber) blkno);
}

static int
ps_punch_hole(int fd, off_t offset, off_t len)
{
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
	return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
					 offset, len);
#elif defined(F_PUNCHHOLE)
	fpunchhole_t args;

	memset(&args, 0, sizeof(args));
	args.fp_offset = offset;
	args.fp_length = len;
	return fcntl(fd, F_PUNCHHOLE, &args);
#else
	errno = EOPNOTSUPP;
	return -1;
#endif
}

/*
 * Punch blocks out of a data file: those set in unwritten, and those set in
 * hidden unless another data directory may share the file.
 */
static void
ps_punch_blocks(uint32 gen, const PsForkKey *key, const uint32 *unwritten,
				const uint32 *hidden, BlockNumber nblocks,
				PageStoreReclaimResult *result)
{
	char		path[MAXPGPATH];
	struct stat st;
	blkcnt_t	before;
	size_t		nwords = ps_bitmap_words(nblocks);
	bool		shared;
	int			fd;

	ps_data_path(path, gen, key);
	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}
	if (fstat(fd, &st) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	before = st.st_blocks;
	shared = st.st_nlink != 1;

#define PS_PUNCHABLE(b) \
	(ps_bitmap_test(unwritten, nwords, (b)) || \
	 (!shared && ps_bitmap_test(hidden, nwords, (b))))

	for (BlockNumber start = 0; start < nblocks;)
	{
		BlockNumber end;
		off_t		data;

		if (!PS_PUNCHABLE(start))
		{
			start++;
			continue;
		}
		for (end = start + 1; end < nblocks && PS_PUNCHABLE(end); end++)
			;

		/* skip what is a hole already, which is most of it after a while */
#ifdef SEEK_DATA
		data = lseek(fd, (off_t) start * BLCKSZ, SEEK_DATA);
		if (data < 0 && errno == ENXIO)
			break;				/* nothing but holes from here on */
		if (data >= (off_t) end * BLCKSZ)
		{
			start = end;
			continue;
		}
#else
		data = 0;
#endif

		if (ps_punch_hole(fd, (off_t) start * BLCKSZ,
						  (off_t) (end - start) * BLCKSZ) < 0)
		{
			/* the file system can't; nothing to reclaim here then */
			if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL)
				break;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not punch blocks %u..%u out of file \"%s\": %m",
							start, end - 1, path)));
		}
		start = end;
	}
#undef PS_PUNCHABLE

	if (fstat(fd, &st) == 0 && st.st_blocks < before)
		result->reclaimed_bytes += (int64) (before - st.st_blocks) * 512;
	CloseTransientFile(fd);
}

static void
ps_add_dead_fork(PsDeadFork **dead, int *ndead, int *maxdead, uint32 gen,
				 const PsForkKey *key)
{
	if (*ndead == *maxdead)
	{
		*maxdead *= 2;
		*dead = repalloc_array(*dead, PsDeadFork, *maxdead);
	}
	(*dead)[*ndead].gen = gen;
	(*dead)[*ndead].key = *key;
	(*ndead)++;
}

/*
 * Work out what of one fork's files in the frozen layers can go.  Hidden
 * blocks are punched out right away; files that hold nothing readable are
 * added to *dead, to be unlinked later.
 */
static void
ps_reclaim_fork(const uint32 *layers, int nlayers, const PsForkKey *key,
				PsDeadFork **dead, int *ndead, int *maxdead,
				PageStoreReclaimResult *result)
{
	bool		hide_all = false;	/* the layers above hide every block */
	uint64		hide_from = PG_UINT64_MAX;	/* ... and every block from here */
	uint32	   *hidden = NULL;	/* ... and these blocks below hide_from */
	size_t		hidden_words = 0;

	for (int i = nlayers - 1; i >= 0; i--)
	{
		PsFrozenFork *f = ps_frozen_lookup(layers[i], key);

		if (f == NULL)
			continue;

		if (hide_all)
		{
			ps_add_dead_fork(dead, ndead, maxdead, layers[i], key);
			continue;
		}

		if (f->kind == PS_FORK_DATA)
		{
			size_t		nwords = Max(ps_bitmap_words(f->nblocks), 1);
			uint32	   *unwritten = palloc0(nwords * sizeof(uint32));
			uint32	   *deadbits = palloc0(nwords * sizeof(uint32));
			bool		anypunch = false;

			/*
			 * Below the topmost layer with files for the fork, which has its
			 * size, see whether any block the layer holds is still readable.
			 * Nothing readable: the layer hides nothing the layers above
			 * don't, so the layers below see the same without it.
			 */
			if (hide_from != PG_UINT64_MAX)
			{
				bool		readable = false;

				for (uint64 blkno = 0; blkno < hide_from && !readable; blkno++)
					readable = ps_frozen_holds(f, blkno) &&
						!ps_bitmap_test(hidden, hidden_words, blkno);
				if (!readable)
				{
					pfree(unwritten);
					pfree(deadbits);
					ps_add_dead_fork(dead, ndead, maxdead, layers[i], key);
					continue;
				}
			}

			for (BlockNumber blkno = 0; blkno < f->nblocks; blkno++)
			{
				if (!ps_frozen_holds(f, blkno))
				{
					unwritten[blkno / 32] |= 1U << (blkno % 32);
					anypunch = true;
				}
				else if (blkno >= hide_from ||
						 ps_bitmap_test(hidden, hidden_words, blkno))
				{
					deadbits[blkno / 32] |= 1U << (blkno % 32);
					anypunch = true;
				}
			}
			if (anypunch && f->has_data)
				ps_punch_blocks(layers[i], key, unwritten, deadbits,
								f->nblocks, result);
			pfree(unwritten);
			pfree(deadbits);
		}

		/* What the layer hides from the layers below */
		if (f->kind == PS_FORK_DROPPED || f->base == 0)
		{
			hide_all = true;
			continue;
		}
		if (ps_bitmap_words(f->base) > hidden_words)
		{
			size_t		words = ps_bitmap_words(f->base);

			hidden = hidden ? repalloc0(hidden, hidden_words * sizeof(uint32),
										words * sizeof(uint32))
				: palloc0(words * sizeof(uint32));
			hidden_words = words;
		}
		for (size_t w = 0; f->bits && w < ps_bitmap_words(f->base); w++)
			hidden[w] |= f->bits[w];
		hide_from = Min(hide_from, (uint64) f->base);
	}

	if (hidden)
		pfree(hidden);
}

/* Unlink a file this cluster no longer reads, noting the space freed. */
static void
ps_unlink_dead_file(const char *path, PageStoreReclaimResult *result)
{
	struct stat st;

	if (lstat(path, &st) < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}
	if (unlink(path) < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
	}
	result->removed_files++;
	if (st.st_nlink == 1)
		result->reclaimed_bytes += (int64) st.st_blocks * 512;
}

static bool
ps_layer_is_empty(uint32 gen)
{
	char		dir[MAXPGPATH];
	DIR		   *dirdesc;
	struct dirent *de;
	bool		empty = true;

	ps_layer_path(dir, gen);
	dirdesc = AllocateDir(dir);
	while (empty && (de = ReadDir(dirdesc, dir)) != NULL)
	{
		PsForkKey	key;
		bool		is_map,
					is_tmp;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* a map a crash kept from being renamed into place doesn't count */
		if (ps_parse_name(de->d_name, &key, &is_map, &is_tmp) && is_tmp)
		{
			char		path[MAXPGPATH];

			snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
			if (unlink(path) == 0 || errno == ENOENT)
				continue;
		}
		empty = false;
	}
	FreeDir(dirdesc);
	return empty;
}

/* Forget what this backend read of a layer that is gone. */
static void
ps_forget_layer(uint32 gen)
{
	HASH_SEQ_STATUS status;
	PsFrozenFork *f;

	if (ps_frozen == NULL)
		return;
	hash_seq_init(&status, ps_frozen);
	while ((f = hash_seq_search(&status)) != NULL)
	{
		if (f->key.gen != gen)
			continue;
		if (f->bits)
			pfree(f->bits);
		hash_search(ps_frozen, &f->key, HASH_REMOVE, NULL);
	}
	hash_search(ps_indexed_gens, &gen, HASH_REMOVE, NULL);
}

static void
ps_reclaim(PageStoreReclaimResult *result, bool wait)
{
	uint32		layers[PAGESTORE_MAX_LAYERS];
	int			nlayers;
	HTAB	   *forks;
	HASHCTL		ctl;
	HASH_SEQ_STATUS status;
	PsFrozenFork *f;
	PsForkKey  *key;
	PsDeadFork *dead;
	int			ndead = 0;
	int			maxdead = 64;
	uint32		emptied[PAGESTORE_MAX_LAYERS];
	int			nemptied = 0;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "page store reclaim",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/*
	 * The frozen layers as of now.  Under the branch lock: pg_branch()
	 * freezes under it, and the frozen layer is durable only once the freeze
	 * returns.
	 */
	if (wait)
		LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE))
		goto done;
	LWLockAcquire(&PsCtl->partition_locks[0].lock, LW_SHARED);
	ps_sync_stack();
	nlayers = ps_local_nlayers - 1;
	memcpy(layers, ps_local_layers, sizeof(uint32) * nlayers);
	LWLockRelease(&PsCtl->partition_locks[0].lock);
	LWLockRelease(&PsCtl->branch_lock);

	/* Every fork any of them has files for */
	ctl.keysize = sizeof(PsForkKey);
	ctl.entrysize = sizeof(PsForkKey);
	ctl.hcxt = cxt;
	forks = hash_create("page store reclaim forks", 1024, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < nlayers; i++)
		ps_index_layer(layers[i]);
	if (ps_frozen)
	{
		hash_seq_init(&status, ps_frozen);
		while ((f = hash_seq_search(&status)) != NULL)
		{
			for (int i = 0; i < nlayers; i++)
			{
				if (f->key.gen == layers[i])
				{
					hash_search(forks, &f->key.fork, HASH_ENTER, NULL);
					break;
				}
			}
		}
	}

	dead = palloc_array(PsDeadFork, maxdead);
	hash_seq_init(&status, forks);
	while ((key = hash_seq_search(&status)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		/* the checkpointer mustn't let its request queue fill up meanwhile */
		if (AmCheckpointerProcess())
			AbsorbSyncRequests();

		ps_reclaim_fork(layers, nlayers, key, &dead, &ndead, &maxdead, result);
	}

	/*
	 * Unlink, and take empty layers off the stack, where pg_branch() can't be
	 * linking them meanwhile.
	 */
	if (wait)
		LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE))
		goto done;

	/*
	 * Data files first, durably, then maps.  A map without its data file is
	 * transparent, as the fork is meant to be; a data file without its map
	 * would own blocks the layers below hold.
	 */
	for (int i = 0; i < ndead; i++)
	{
		char		path[MAXPGPATH];

		ps_data_path(path, dead[i].gen, &dead[i].key);
		ps_unlink_dead_file(path, result);
	}
	for (int i = 0; i < nlayers; i++)
	{
		for (int j = 0; j < ndead; j++)
		{
			if (dead[j].gen == layers[i])
			{
				char		dir[MAXPGPATH];

				ps_layer_path(dir, layers[i]);
				fsync_fname(dir, true);
				break;
			}
		}
	}
	for (int i = 0; i < ndead; i++)
	{
		char		path[MAXPGPATH];
		PsFrozenKey fkey;

		ps_map_path(path, dead[i].gen, &dead[i].key);
		ps_unlink_dead_file(path, result);

		/* this backend now sees the layer as transparent for the fork */
		memset(&fkey, 0, sizeof(fkey));
		fkey.gen = dead[i].gen;
		fkey.fork = dead[i].key;
		f = hash_search(ps_frozen, &fkey, HASH_FIND, NULL);
		if (f)
		{
			if (f->bits)
				pfree(f->bits);
			f->bits = NULL;
			f->loaded = true;
			f->absent = true;
		}
	}

	/* Any frozen layer may be empty, also after a crash here */
	for (int i = 0; i < nlayers; i++)
	{
		if (ps_layer_is_empty(layers[i]))
			emptied[nemptied++] = layers[i];
	}

	if (nemptied > 0)
	{
		uint32		newlayers[PAGESTORE_MAX_LAYERS];
		int			nnew = 0;

		ps_acquire_all_partitions(LW_EXCLUSIVE);
		for (int i = 0; i < PsCtl->nlayers; i++)
		{
			bool		keep = true;

			/* never the top layer */
			for (int j = 0; j < nemptied && i < PsCtl->nlayers - 1; j++)
				keep &= PsCtl->layers[i] != emptied[j];
			if (keep)
				newlayers[nnew++] = PsCtl->layers[i];
		}
		PageStoreWriteStackFile(NULL, newlayers, nnew);
		memcpy(PsCtl->layers, newlayers, sizeof(uint32) * nnew);
		result->removed_layers += PsCtl->nlayers - nnew;
		PsCtl->nlayers = nnew;
		PsCtl->stack_version++;
		ps_release_all_partitions();

		for (int i = 0; i < nemptied; i++)
		{
			char		dir[MAXPGPATH];

			ps_layer_path(dir, emptied[i]);
			if (rmdir(dir) < 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove directory \"%s\": %m", dir)));
			ps_forget_layer(emptied[i]);
		}
		fsync_fname(PAGESTORE_DIR, true);
	}

	LWLockRelease(&PsCtl->branch_lock);

done:
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

static void
ps_reclaim_cleanup(int code, Datum arg)
{
	pg_atomic_clear_flag(&PsCtl->reclaim_running);
}

/*
 * Give back the space of frozen layers that this cluster can no longer read.
 * If wait is true, wait for another process reclaiming already, and for
 * pg_branch(); if false, return false if another process is reclaiming, and
 * skip the steps that would wait for pg_branch().
 */
bool
PageStoreReclaim(PageStoreReclaimResult *result, bool wait)
{
	memset(result, 0, sizeof(PageStoreReclaimResult));

	if (!PageStoreEnabled())
		return true;
	while (!pg_atomic_test_set_flag(&PsCtl->reclaim_running))
	{
		if (!wait)
			return false;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(10000L);
	}

	/* also when the process exits, as on FATAL */
	PG_ENSURE_ERROR_CLEANUP(ps_reclaim_cleanup, (Datum) 0);
	{
		ps_reclaim(result, wait);
	}
	PG_END_ENSURE_ERROR_CLEANUP(ps_reclaim_cleanup, (Datum) 0);
	pg_atomic_clear_flag(&PsCtl->reclaim_running);

	return true;
}

/*
 * Called by the checkpointer after each checkpoint.  New frozen layers make
 * more space reclaimable, and so does a branch going away (its hard links
 * with it), which nothing tells us about: so reclaim when the stack has
 * changed, and every few checkpoints anyway.
 */
void
PageStoreReclaimAfterCheckpoint(void)
{
	static uint32 last_stack_version = PG_UINT32_MAX;
	static int	skipped = 0;
	PageStoreReclaimResult result;

	if (!PageStoreEnabled() || !pagestore_reclaim)
		return;
	if (PsCtl->stack_version == last_stack_version && ++skipped < 8)
		return;
	skipped = 0;
	last_stack_version = PsCtl->stack_version;

	if (PageStoreReclaim(&result, false) &&
		(result.removed_files > 0 || result.reclaimed_bytes > 0))
		ereport(LOG,
				(errmsg("page store reclaimed %lld kB, removing %lld files and %d layers",
						(long long) (result.reclaimed_bytes / 1024),
						(long long) result.removed_files,
						result.removed_layers)));
}
