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
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#if defined(__darwin__)
#include <sys/clonefile.h>
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
#include "port/pg_iovec.h"
#include "postmaster/bgworker.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/md.h"
#include "storage/pagestore.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/reinit.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/* GUCs */
int			pagestore_max_forks = 65536;
int			pagestore_max_bitmap_chunks = 16384;
bool		pagestore_reclaim = true;
int			pagestore_max_frozen_layers = 8;
bool		pagestore_auto_freeze = true;

#define PS_NUM_PARTITIONS	16

/* A bitmap chunk covers this many blocks */
#define PS_CHUNK_BLOCKS		8192
#define PS_CHUNK_WORDS		(PS_CHUNK_BLOCKS / 32)

/*
 * A fork whose base is this small keeps its bits in its entry rather than in
 * chunks: most forks are this small, and a chunk would take 1 kB for each.
 */
#define PS_SMALL_BLOCKS		256
#define PS_SMALL_WORDS		(PS_SMALL_BLOCKS / 32)

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

	/*
	 * Whether the bits below base are in small_bits rather than in chunks.
	 * Set when the entry gets its base, which only shrinks from then on.
	 */
	bool		small_bitmap;
	uint32		small_bits[PS_SMALL_WORDS];
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

	/*
	 * Bumped when frozen layers gain files, which CREATE DATABASE ...
	 * STRATEGY SNAPSHOT gives them, so that backends read them again. Changed
	 * while holding all partition locks, like the stack.
	 */
	uint32		index_version;

	/*
	 * The generation of the next layer to make.  A generation is never reused
	 * while the server runs, because backends cache what they read of frozen
	 * layers by generation.  The stack's order is that of the STACK file, not
	 * that of the generations: merging puts a new one low.
	 */
	uint32		next_gen;

	/* WAL from here on is being copied into a branch, see KeepLogSeg() */
	pg_atomic_uint64 branch_keep_lsn;

	/* set while PageStoreReclaim() or PageStoreMerge() runs */
	pg_atomic_flag reclaim_running;

	/* set while a process removes merged layers' trash */
	pg_atomic_flag trash_emptying;

	/* entries in PsTopForks, see ps_top_has_room(), and in PsChunks */
	pg_atomic_uint32 top_forks;
	pg_atomic_uint32 top_chunks;

	/*
	 * Set when either passes PS_AUTO_FREEZE_PERCENT, which wakes the page
	 * store merger to freeze the top layer and so empty them.
	 */
	pg_atomic_flag freeze_requested;
	ProcNumber	merger_procno;

	/*
	 * CREATE DATABASE ... STRATEGY SNAPSHOT calls waiting for that flag.
	 * Reclaiming and merging in the background stop early for them.
	 */
	pg_atomic_uint32 clones_waiting;

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
static uint32 ps_local_index_version = 0;
static int	ps_local_nlayers = 0;
static uint32 ps_local_layers[PAGESTORE_MAX_LAYERS];

#define PS_TOPGEN()		(ps_local_layers[ps_local_nlayers - 1])

static void ps_forget_layer(uint32 gen);
static void ps_forget_all_layers(void);
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
static void ps_remove_orphan_layers(void);
static void ps_load_top_layer(void);

static void
PageStoreShmemInit(void *arg)
{
	memset(PsCtl, 0, sizeof(PsShared));
	pg_atomic_init_u64(&PsCtl->branch_keep_lsn, InvalidXLogRecPtr);
	pg_atomic_init_flag(&PsCtl->reclaim_running);
	pg_atomic_init_flag(&PsCtl->trash_emptying);
	pg_atomic_init_u32(&PsCtl->top_forks, 0);
	pg_atomic_init_u32(&PsCtl->top_chunks, 0);
	pg_atomic_init_flag(&PsCtl->freeze_requested);
	PsCtl->merger_procno = INVALID_PROC_NUMBER;
	pg_atomic_init_u32(&PsCtl->clones_waiting, 0);
	LWLockInitialize(&PsCtl->freeze_lock, LWTRANCHE_PAGESTORE);
	LWLockInitialize(&PsCtl->branch_lock, LWTRANCHE_PAGESTORE);
	for (int i = 0; i < PS_NUM_PARTITIONS; i++)
		LWLockInitialize(&PsCtl->partition_locks[i].lock, LWTRANCHE_PAGESTORE);

	if (PsTopForks == NULL)
		return;

	PsCtl->enabled = true;
	ps_read_stack(PsCtl->layers, &PsCtl->nlayers);
	ps_remove_orphan_layers();
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
		ps_local_stack_version == PsCtl->stack_version &&
		ps_local_index_version == PsCtl->index_version)
		return;

	ps_local_nlayers = PsCtl->nlayers;
	memcpy(ps_local_layers, PsCtl->layers,
		   sizeof(uint32) * ps_local_nlayers);
	ps_local_stack_version = PsCtl->stack_version;

	/* Forget layers that merging or reclaiming took off the stack */
	if (ps_indexed_gens)
	{
		HASH_SEQ_STATUS status;
		uint32	   *gen;
		uint32		gone[PAGESTORE_MAX_LAYERS];
		int			ngone = 0;

		hash_seq_init(&status, ps_indexed_gens);
		while ((gen = hash_seq_search(&status)) != NULL)
		{
			bool		listed = false;

			for (int i = 0; i < ps_local_nlayers; i++)
				listed |= ps_local_layers[i] == *gen;
			if (!listed && ngone < PAGESTORE_MAX_LAYERS)
				gone[ngone++] = *gen;
		}
		for (int i = 0; i < ngone; i++)
			ps_forget_layer(gone[i]);
	}

	/* Read again the frozen layers that gained files */
	if (ps_local_index_version != PsCtl->index_version)
	{
		ps_local_index_version = PsCtl->index_version;
		ps_forget_all_layers();
	}
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
			*nlayers >= PAGESTORE_MAX_LAYERS)
			ereport(FATAL,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid contents in file \"%s\"", PAGESTORE_STACK_FILE)));
		for (int i = 0; i < *nlayers; i++)
		{
			if (layers[i] == gen)
				ereport(FATAL,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid contents in file \"%s\"", PAGESTORE_STACK_FILE)));
		}
		layers[(*nlayers)++] = (uint32) gen;
		p = end;
	}
	if (*nlayers == 0)
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid contents in file \"%s\"", PAGESTORE_STACK_FILE)));
}

/*
 * Remove layer directories the stack doesn't list: what a crash left of a
 * freeze or merge that didn't get to update the stack, or of a merge that
 * did but hadn't removed the layers it replaced.  Also set next_gen past
 * every generation in use.  Runs while setting up shared memory.
 */
static void
ps_remove_orphan_layers(void)
{
	DIR		   *dirdesc;
	struct dirent *de;
	uint32		maxgen = 0;

	for (int i = 0; i < PsCtl->nlayers; i++)
		maxgen = Max(maxgen, PsCtl->layers[i]);

	dirdesc = AllocateDir(PAGESTORE_DIR);
	while ((de = ReadDir(dirdesc, PAGESTORE_DIR)) != NULL)
	{
		char	   *end;
		unsigned long gen;
		bool		listed = false;
		char		path[MAXPGPATH];

		gen = strtoul(de->d_name, &end, 10);
		if (end == de->d_name || gen == 0 || gen > PG_UINT32_MAX)
			continue;
		maxgen = Max(maxgen, (uint32) gen);

		for (int i = 0; i < PsCtl->nlayers; i++)
			listed |= PsCtl->layers[i] == gen;
		if (listed)
			continue;

		/* "<gen>" or "<gen>.tmp", made by a merge */
		snprintf(path, sizeof(path), "%s/%s", PAGESTORE_DIR, de->d_name);
		ereport(LOG,
				(errmsg("removing page store layer \"%s\" that is not in the stack",
						path)));
		if (!rmtree(path, true))
			ereport(FATAL,
					(errmsg("could not remove directory \"%s\"", path)));
	}
	FreeDir(dirdesc);

	PsCtl->next_gen = maxgen + 1;
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

/* How full the top layer's tables get before it's frozen to empty them */
#define PS_AUTO_FREEZE_PERCENT	75

/*
 * Ask the page store merger to freeze the top layer if one of its tables is
 * getting full.  Cheap enough to call on every insert.
 */
static void
ps_maybe_request_freeze(uint32 used, int max)
{
	ProcNumber	merger;

	if (!pagestore_auto_freeze ||
		(uint64) used * 100 < (uint64) max * PS_AUTO_FREEZE_PERCENT)
		return;
	if (!pg_atomic_test_set_flag(&PsCtl->freeze_requested))
		return;					/* asked already */
	merger = PsCtl->merger_procno;
	if (merger != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(merger)->procLatch);
}

/*
 * Should reclaiming or merging in the background stop early?  When a clone
 * waits for them, or the top layer is to be frozen, which the merger does
 * between merges.
 */
static inline bool
ps_background_should_yield(void)
{
	return pg_atomic_read_u32(&PsCtl->clones_waiting) > 0 ||
		!pg_atomic_unlocked_test_flag(&PsCtl->freeze_requested);
}

/*
 * Fork entries kept for what can't fail: truncating, which runs in a critical
 * section, dropping, which runs after commit, and loading the top layer at
 * startup.  Everything else checks for room with ps_top_has_room() before it
 * changes anything, and fails without holding locks once only these are
 * left: a backend that errors while holding a partition lock can't take it
 * again in its error cleanup, and others queue up behind it.
 */
#define PS_RESERVED_FORKS	Max(64, pagestore_max_forks / 64)

/*
 * Is there room for the fork's top layer entry, if it needs one?  Caller
 * holds the fork's partition lock.
 */
static bool
ps_top_has_room(const PsForkKey *key, uint32 hash, bool critical)
{
	if (critical || ps_top_lookup(key, hash) != NULL)
		return true;
	return pg_atomic_read_u32(&PsCtl->top_forks) + PS_RESERVED_FORKS <
		(uint32) pagestore_max_forks;
}

/* Report that the top layer is full.  Caller holds no page store lock. */
static void
ps_report_full(const char *what, const char *guc)
{
	ereport(ERROR,
			(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
			 errmsg("out of page store %s", what),
			 errhint("Freezing the top layer with pg_pagestore_freeze() frees them, or you might need to increase \"%s\".",
					 guc)));
}

/*
 * Make room in the top layer's tables by freezing it, for a write that found
 * them full: what pagestore_auto_freeze has the merger do earlier, should it
 * not keep up.  The checkpointer and the startup process always do: a
 * failed shutdown checkpoint would leave crash recovery for the next start,
 * and a failed recovery wouldn't start.  Returns false if it can't: auto
 * freezing is off, this backend holds the branch lock, as a clone copying
 * does, or it tried a few times already for the same write (*attempts
 * counts).  Caller holds no other page store lock.
 */
static bool
ps_freeze_for_room(int *attempts)
{
	uint32		layers[PAGESTORE_MAX_LAYERS];
	XLogRecPtr	insert_end;

	if (!(pagestore_auto_freeze || AmCheckpointerProcess() ||
		  AmStartupProcess()) ||
		LWLockHeldByMe(&PsCtl->branch_lock) || ++*attempts > 3)
		return false;

	LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	/* unless another backend did meanwhile */
	if (pg_atomic_read_u32(&PsCtl->top_forks) + PS_RESERVED_FORKS >=
		(uint32) pagestore_max_forks ||
		pg_atomic_read_u32(&PsCtl->top_chunks) * 100 >=
		(uint32) pagestore_max_bitmap_chunks * PS_AUTO_FREEZE_PERCENT)
	{
		(void) PageStoreFreeze(layers, &insert_end);
		ereport(LOG,
				(errmsg("page store froze its top layer, which was full")));
	}
	LWLockRelease(&PsCtl->branch_lock);
	return true;
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
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of page store fork entries"),
				 errhint("You might need to increase \"%s\".",
						 "pagestore_max_forks")));
	if (!found)
	{
		ps_maybe_request_freeze(pg_atomic_add_fetch_u32(&PsCtl->top_forks, 1),
								pagestore_max_forks);
		top->kind = PS_FORK_DATA;
		top->has_map = false;
		top->map_dirty = false;
		top->unsynced = false;
		top->map_version = 0;
		top->base = 0;
		top->small_bitmap = true;
		memset(top->small_bits, 0, sizeof(top->small_bits));
	}
	return top;
}

/* Give a new entry the base it inherits; see small_bitmap. */
static void
ps_top_set_initial_base(PsTopFork *top, BlockNumber base)
{
	top->base = base;
	top->small_bitmap = base <= PS_SMALL_BLOCKS;
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
	if (create && chunk != NULL && !found)
	{
		memset(chunk->bits, 0, sizeof(chunk->bits));
		ps_maybe_request_freeze(pg_atomic_add_fetch_u32(&PsCtl->top_chunks, 1),
								pagestore_max_bitmap_chunks);
	}
	return chunk;
}

static bool
ps_top_bit(const PsTopFork *top, uint32 hash, BlockNumber blkno)
{
	PsChunk    *chunk;
	uint32		off = blkno % PS_CHUNK_BLOCKS;

	if (top->small_bitmap)
		return blkno < PS_SMALL_BLOCKS &&
			(top->small_bits[blkno / 32] & (1U << (blkno % 32))) != 0;

	chunk = ps_chunk_lookup(&top->key, hash, blkno / PS_CHUNK_BLOCKS, false);
	return chunk != NULL && (chunk->bits[off / 32] & (1U << (off % 32))) != 0;
}

static void
ps_top_set_bit(PsTopFork *top, uint32 hash, BlockNumber blkno)
{
	PsChunk    *chunk;
	uint32		off = blkno % PS_CHUNK_BLOCKS;

	if (top->small_bitmap)
	{
		Assert(blkno < PS_SMALL_BLOCKS);
		top->small_bits[blkno / 32] |= 1U << (blkno % 32);
		return;
	}

	chunk = ps_chunk_lookup(&top->key, hash, blkno / PS_CHUNK_BLOCKS, true);
	if (chunk == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of page store bitmap chunks"),
				 errhint("You might need to increase \"%s\".",
						 "pagestore_max_bitmap_chunks")));
	chunk->bits[off / 32] |= 1U << (off % 32);
}

/*
 * Have the bitmap chunks for blocks from up to to of the fork's top layer
 * entry, so that setting their bits can't fail.  Returns false if there
 * aren't enough.  Caller holds the freeze lock, so they stay.
 */
static bool
ps_top_reserve_chunks(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber from, BlockNumber to)
{
	PsForkKey	key;
	uint32		hash;
	LWLock	   *lock;
	PsTopFork  *top;
	bool		ok = true;

	ps_key_init(&key, reln->smgr_rlocator.locator, forknum);
	hash = ps_fork_hash(&key);
	lock = ps_partition_lock(hash);

	LWLockAcquire(lock, LW_EXCLUSIVE);
	top = ps_top_lookup(&key, hash);
	for (uint32 chunkno = from / PS_CHUNK_BLOCKS;
		 ok && (top == NULL || !top->small_bitmap) &&
		 (uint64) chunkno * PS_CHUNK_BLOCKS < to;
		 chunkno++)
		ok = ps_chunk_lookup(&key, hash, chunkno, true) != NULL;
	LWLockRelease(lock);
	return ok;
}

/* Clear the bits at or above from, below the fork's old base. */
static void
ps_top_clear_bits(PsTopFork *top, uint32 hash, BlockNumber from,
				  BlockNumber oldbase)
{
	const PsForkKey *key = &top->key;

	if (top->small_bitmap)
	{
		for (BlockNumber blkno = from; blkno < Min(oldbase, PS_SMALL_BLOCKS);
			 blkno++)
			top->small_bits[blkno / 32] &= ~(1U << (blkno % 32));
		return;
	}

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
			pg_atomic_fetch_sub_u32(&PsCtl->top_chunks, 1);
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

	ps_top_clear_bits(top, hash, 0, top->base);
	hash_search_with_hash_value(PsTopForks, &key, hash, HASH_REMOVE, NULL);
	pg_atomic_fetch_sub_u32(&PsCtl->top_forks, 1);
}

/* Copy a top layer fork's bitmap below base out of shared memory. */
static uint32 *
ps_top_snapshot_bits(const PsTopFork *top, uint32 hash, BlockNumber base)
{
	const PsForkKey *key = &top->key;
	size_t		nwords = ps_bitmap_words(base);
	uint32	   *bits;

	bits = MemoryContextAllocZero(PsCxt ? PsCxt : CurrentMemoryContext,
								  Max(nwords, 1) * sizeof(uint32));
	if (top->small_bitmap)
	{
		memcpy(bits, top->small_bits,
			   Min(nwords, PS_SMALL_WORDS) * sizeof(uint32));
		return bits;
	}
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
		ps_top_set_initial_base(top, base);
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
					ps_top_set_bit(top, hash, blkno);
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
		if (blkno >= top->base || ps_top_bit(top, hash, blkno))
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
 * if it has none yet.  Errors out if the fork doesn't exist.  Returns NULL if
 * the top layer has no room for the fork, unless critical; the caller then
 * lets go of its locks and calls ps_report_full().  Caller holds the freeze
 * lock in shared mode, and the fork's partition lock exclusively.
 */
static PsTopFork *
ps_ensure_top(SMgrRelation reln, ForkNumber forknum, const PsForkKey *key,
			  uint32 hash, bool critical)
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
	if (!ps_top_has_room(key, hash, critical))
		return NULL;

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
	ps_top_set_initial_base(top, lower->nblocks);
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
	int			attempts;
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

	attempts = 0;
retry:
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

	if (!ps_top_has_room(&key, hash, isRedo))
	{
		LWLockRelease(lock);
		LWLockRelease(&PsCtl->freeze_lock);
		if (ps_freeze_for_room(&attempts))
			goto retry;
		ps_report_full("fork entries", "pagestore_max_forks");
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
	ps_top_set_initial_base(top, 0);
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
				ps_top_clear_bits(top, hash, 0, top->base);
			else
				top = ps_top_insert(&key, hash);
			top->kind = PS_FORK_DROPPED;
			ps_top_set_initial_base(top, 0);
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
			ps_top_clear_bits(top, hash, 0, top->base);
			ps_top_set_initial_base(top, 0);
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

	for (int attempts = 0;;)
	{
		LWLockAcquire(&PsCtl->freeze_lock, LW_SHARED);
		LWLockAcquire(lock, LW_EXCLUSIVE);
		ps_sync_stack();
		top = ps_ensure_top(reln, forknum, &key, hash, false);
		if (top != NULL)
			break;
		LWLockRelease(lock);
		LWLockRelease(&PsCtl->freeze_lock);
		if (!ps_freeze_for_room(&attempts))
			ps_report_full("fork entries", "pagestore_max_forks");
	}
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

	/*
	 * Blocks below the base need bits in the bitmap chunks once written. Have
	 * the chunks before writing, so that running out fails the write, or
	 * freezes to make room.
	 */
	for (int attempts = 0;;)
	{
		gen = ps_begin_write(reln, forknum, &base);
		if (start >= base ||
			ps_top_reserve_chunks(reln, forknum, start,
								  Min((uint64) start + total, (uint64) base)))
			break;
		LWLockRelease(&PsCtl->freeze_lock);
		if (!ps_freeze_for_room(&attempts))
			ps_report_full("bitmap chunks", "pagestore_max_bitmap_chunks");
	}

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
				ps_top_set_bit(top, hash, b);
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
	/* in a critical section: may use the reserved entries */
	top = ps_ensure_top(reln, forknum, &key, hash, true);
	top->unsynced = true;
	gen = PS_TOPGEN();
	if (nblocks < top->base)
	{
		ps_top_clear_bits(top, hash, nblocks, top->base);
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
				bits = ps_top_snapshot_bits(top, hash, top->base);
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
		{
			struct stat st;

			if (stat(dir, &st) < 0 && errno == ENOENT)
				return 0;
		}
		if (fsync_fname_ext(dir, true, false, LOG) != 0)
			return -1;
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
		bits = ps_top_snapshot_bits(top, hash, top->base);
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
 * Does any layer have files of the database?  Like a directory left behind
 * in a tablespace, the files of a dropped database, or of one whose creation
 * failed, keep its OID from being used again until they're gone.
 */
bool
PageStoreDatabaseInUse(Oid dbid)
{
	PsForkKey  *forks;
	int			nforks;

	if (!PageStoreEnabled())
		return false;

	forks = ps_list_forks(false, dbid, &nforks);
	pfree(forks);
	return nforks > 0;
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
			uint32	   *bits = ps_top_snapshot_bits(top, hash, top->base);

			ps_write_map(oldtop, &top->key, 0, top->base, bits, false, ERROR);
			pfree(bits);
			map_written = true;
		}
		if (top->unsynced || map_written)
			synckeys[nsync++] = top->key;
	}

	/* start the new top layer, in a directory a failed freeze may have left */
	newtop = PsCtl->next_gen++;
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
	fsync_fname(PAGESTORE_DIR, true);

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
	pg_atomic_write_u32(&PsCtl->top_forks, 0);
	pg_atomic_write_u32(&PsCtl->top_chunks, 0);

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
 * Databases sharing a template's files
 *
 * CREATE DATABASE ... STRATEGY SNAPSHOT gives each of the template's files in
 * a frozen layer a second name, the new database's, in the same layer.
 * Frozen files never change, so neither database sees what the other writes
 * from then on: that goes to its own files in the top layer.  Reclaiming
 * leaves the blocks of a file with more than one name alone, so a block one
 * of them no longer reads stays for the other, and dropping either removes
 * only its own names.
 *
 * The template's writes since the last freeze are in the top layer, which
 * still changes.  A few are copied into the new database's own files in the
 * top layer; more, or more than the top layer's tables have room for, and
 * the top layer is frozen first instead.  Freezing
 * fsyncs what the whole cluster wrote since its last checkpoint, which can
 * take long, so it's worth copying a little to avoid it.  Reading a database
 * counts as writing it, as reads set hint bits.
 */

/* Most of the template's top layer data to copy rather than freeze */
#define PS_SNAPSHOT_COPY_BYTES	(16 * 1024 * 1024)

static bool ps_bitmap_test(const uint32 *bits, size_t nwords, uint64 blkno);
static int64 ps_allocated_bytes(uint32 gen, const PsForkKey *key);

/* A name given to one of the template's files */
typedef struct PsSnapshotLink
{
	uint32		gen;
	PsForkKey	key;			/* the new database's */
	bool		is_map;
} PsSnapshotLink;

typedef struct PsSnapshotState
{
	PsSnapshotLink *links;		/* made so far, to take back on error */
	int			nlinks;
	int			maxlinks;
	bool		holding_flag;	/* holds reclaim_running */
	bool		waiting;		/* counted in clones_waiting */
} PsSnapshotState;

static void
ps_name_path(char *path, uint32 gen, const PsForkKey *key, bool is_map)
{
	if (is_map)
		ps_map_path(path, gen, key);
	else
		ps_data_path(path, gen, key);
}

/* Make backends read frozen layers again; see index_version. */
static void
ps_bump_index_version(void)
{
	ps_acquire_all_partitions(LW_EXCLUSIVE);
	PsCtl->index_version++;
	ps_release_all_partitions();
}

static void
ps_snapshot_cleanup(int code, Datum arg)
{
	PsSnapshotState *state = (PsSnapshotState *) DatumGetPointer(arg);

	/*
	 * The error may have come while holding page store locks, which this
	 * takes again.  Let go of them first, as aborting would.
	 */
	LWLockReleaseAll();

	/* data files before their maps, the reverse of linking */
	for (int i = state->nlinks - 1; i >= 0; i--)
	{
		char		path[MAXPGPATH];

		ps_name_path(path, state->links[i].gen, &state->links[i].key,
					 state->links[i].is_map);
		if (unlink(path) < 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
	}
	if (state->nlinks > 0)
		ps_bump_index_version();
	if (state->waiting)
		pg_atomic_fetch_sub_u32(&PsCtl->clones_waiting, 1);
	if (state->holding_flag)
		pg_atomic_clear_flag(&PsCtl->reclaim_running);
}

/* Give the template's files in one frozen layer the new database's names. */
static void
ps_snapshot_layer(uint32 gen, Oid src_dbid, Oid dst_dbid,
				  PsSnapshotState *state)
{
	char		dir[MAXPGPATH];
	DIR		   *dirdesc;
	struct dirent *de;
	PsSnapshotLink *names = NULL;
	int			nnames = 0;
	int			maxnames = 0;
	int			first = state->nlinks;

	ps_layer_path(dir, gen);
	dirdesc = AllocateDir(dir);
	while ((de = ReadDir(dirdesc, dir)) != NULL)
	{
		PsForkKey	key;
		bool		is_map;
		bool		is_tmp;

		if (!ps_parse_name(de->d_name, &key, &is_map, &is_tmp) || is_tmp ||
			key.rlocator.spcOid != DEFAULTTABLESPACE_OID ||
			key.rlocator.dbOid != src_dbid)
			continue;
		if (nnames == maxnames)
		{
			maxnames = Max(64, maxnames * 2);
			names = names ? repalloc_array(names, PsSnapshotLink, maxnames) :
				palloc_array(PsSnapshotLink, maxnames);
		}
		names[nnames].gen = gen;
		names[nnames].key = key;
		names[nnames].is_map = is_map;
		nnames++;
	}
	FreeDir(dirdesc);

	/* maps first: a data file without its map would hide the layers below */
	for (int pass = 0; pass < 2; pass++)
	{
		for (int i = 0; i < nnames; i++)
		{
			PsSnapshotLink *name = &names[i];
			char		from[MAXPGPATH];
			char		to[MAXPGPATH];
			PsForkKey	dst = name->key;

			if (name->is_map != (pass == 0))
				continue;
			CHECK_FOR_INTERRUPTS();

			dst.rlocator.dbOid = dst_dbid;
			ps_name_path(from, gen, &name->key, name->is_map);
			ps_name_path(to, gen, &dst, name->is_map);
			if (link(from, to) < 0)
			{
				/* the leftover of a dropped fork, unlinked by a checkpoint */
				if (errno == ENOENT)
					continue;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not link file \"%s\" to \"%s\": %m",
								from, to)));
			}

			if (state->nlinks == state->maxlinks)
			{
				state->maxlinks = Max(256, state->maxlinks * 2);
				state->links = state->links ?
					repalloc_array(state->links, PsSnapshotLink, state->maxlinks) :
					palloc_array(PsSnapshotLink, state->maxlinks);
			}
			state->links[state->nlinks].gen = gen;
			state->links[state->nlinks].key = dst;
			state->links[state->nlinks].is_map = name->is_map;
			state->nlinks++;
		}
	}
	if (names)
		pfree(names);

	if (state->nlinks > first)
		fsync_fname(dir, true);
}

/*
 * Copy one of the template's forks in the top layer into the new database's
 * fork, which has the template's frozen files already: the blocks the top
 * layer holds, and the size.  Durably, as none of this is WAL-logged.
 */
static void
ps_snapshot_copy_fork(const PsForkKey *src_key, Oid dst_dbid)
{
	RelFileLocator dst_rlocator = src_key->rlocator;
	ForkNumber	forknum = src_key->forknum;
	SMgrRelation src;
	SMgrRelation dst;
	PsForkKey	dst_key = *src_key;
	uint32		hash = ps_fork_hash(src_key);
	LWLock	   *lock = ps_partition_lock(hash);
	PsTopFork  *top;
	uint8		kind = PS_FORK_DROPPED;
	BlockNumber base = 0;
	uint32	   *bits = NULL;
	uint32		topgen;
	BlockNumber src_nblocks;
	BlockNumber dst_nblocks;
	PGIOAlignedBlock buf;

	dst_rlocator.dbOid = dst_dbid;
	dst_key.rlocator.dbOid = dst_dbid;

	LWLockAcquire(lock, LW_SHARED);
	ps_sync_stack();
	topgen = PS_TOPGEN();
	top = ps_top_lookup(src_key, hash);
	if (top)
	{
		kind = top->kind;
		base = top->base;
		if (kind == PS_FORK_DATA && base > 0)
			bits = ps_top_snapshot_bits(top, hash, base);
	}
	LWLockRelease(lock);
	if (top == NULL)
		return;

	src = smgropen(src_key->rlocator, INVALID_PROC_NUMBER);
	dst = smgropen(dst_rlocator, INVALID_PROC_NUMBER);

	/* Dropped since the last freeze, over the frozen files */
	if (kind == PS_FORK_DROPPED)
	{
		if (smgrexists(dst, forknum))
			ps_unlink_fork(dst_rlocator, forknum, true);
		return;
	}

	src_nblocks = smgrnblocks(src, forknum);
	if (smgrexists(dst, forknum))
		dst_nblocks = smgrnblocks(dst, forknum);
	else
	{
		smgrcreate(dst, forknum, false);
		dst_nblocks = 0;
	}
	if (dst_nblocks > src_nblocks)
	{
		pstruncate(dst, forknum, dst_nblocks, src_nblocks);
		dst_nblocks = src_nblocks;
	}

	/*
	 * Blocks the top layer doesn't hold are below its base, where the
	 * template's frozen files, now the new database's too, have them.
	 */
	for (BlockNumber blkno = 0; blkno < src_nblocks; blkno++)
	{
		if (blkno < base && !ps_bitmap_test(bits, ps_bitmap_words(base), blkno))
			continue;
		if (blkno > dst_nblocks)
			elog(ERROR, "page store fork of database %u has a gap at block %u",
				 src_key->rlocator.dbOid, dst_nblocks);

		smgrread(src, forknum, blkno, buf.data);
		if (blkno < dst_nblocks)
			smgrwrite(dst, forknum, blkno, buf.data, true);
		else
		{
			smgrextend(dst, forknum, blkno, buf.data, true);
			dst_nblocks++;
		}
	}
	if (bits)
		pfree(bits);

	/* the data, then the map saying which blocks the top layer has */
	ps_immedsync_layer(&dst_key, topgen);
}

/*
 * Give a new database the relations of a template in pg_default, sharing
 * their files.  The caller makes sure that nothing writes to the template
 * meanwhile, and has written out its buffers.  Returns once the new names are
 * durable.  On error, takes back the names made.
 */
void
PageStoreSnapshotDatabase(Oid src_dbid, Oid dst_dbid)
{
	PsSnapshotState state;
	uint32		layers[PAGESTORE_MAX_LAYERS];
	int			nlayers;
	uint32		topgen;
	PsForkKey  *src_top;
	int			nsrc_top = 0;
	int64		src_top_bytes = 0;
	HASH_SEQ_STATUS status;
	PsTopFork  *top;

	if (!PageStoreEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the cluster does not use the page store")));

	memset(&state, 0, sizeof(state));
	PG_ENSURE_ERROR_CLEANUP(ps_snapshot_cleanup, PointerGetDatum(&state));
	{
		/*
		 * Merging and reclaiming change frozen layers; wait for them.  In the
		 * background, they stop early for us.
		 */
		pg_atomic_fetch_add_u32(&PsCtl->clones_waiting, 1);
		state.waiting = true;
		while (!pg_atomic_test_set_flag(&PsCtl->reclaim_running))
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
		}
		state.holding_flag = true;
		pg_atomic_fetch_sub_u32(&PsCtl->clones_waiting, 1);
		state.waiting = false;

		/*
		 * And pg_branch() links their files elsewhere: let it see all of the
		 * new names, or none.
		 */
		LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);

		/* The template's forks in the top layer */
		src_top = palloc_array(PsForkKey, pagestore_max_forks);
		ps_acquire_all_partitions(LW_SHARED);
		hash_seq_init(&status, PsTopForks);
		while ((top = hash_seq_search(&status)) != NULL)
		{
			if (top->key.rlocator.dbOid == src_dbid &&
				top->key.rlocator.spcOid == DEFAULTTABLESPACE_OID)
				src_top[nsrc_top++] = top->key;
		}
		nlayers = PsCtl->nlayers - 1;
		memcpy(layers, PsCtl->layers, sizeof(uint32) * nlayers);
		topgen = PsCtl->layers[PsCtl->nlayers - 1];
		ps_release_all_partitions();

		/*
		 * Writes to the top layer can't make the template's files there grow
		 * meanwhile: nobody writes to the template.
		 */
		for (int i = 0; i < nsrc_top; i++)
			src_top_bytes += ps_allocated_bytes(topgen, &src_top[i]);

		/*
		 * Copying takes a fork entry, and maybe a bitmap chunk, for each fork
		 * copied.  Freeze rather than fill the tables: the merger can't
		 * freeze for us while we hold the branch lock.
		 */
		if (nsrc_top > 0 &&
			(src_top_bytes > PS_SNAPSHOT_COPY_BYTES ||
			 (uint64) (pg_atomic_read_u32(&PsCtl->top_forks) + nsrc_top) * 100 >=
			 (uint64) pagestore_max_forks * PS_AUTO_FREEZE_PERCENT ||
			 (uint64) (pg_atomic_read_u32(&PsCtl->top_chunks) + nsrc_top) * 100 >=
			 (uint64) pagestore_max_bitmap_chunks * PS_AUTO_FREEZE_PERCENT))
		{
			XLogRecPtr	insert_end;

			nlayers = PageStoreFreeze(layers, &insert_end);
			nsrc_top = 0;
		}

		for (int i = 0; i < nlayers; i++)
			ps_snapshot_layer(layers[i], src_dbid, dst_dbid, &state);
		if (state.nlinks > 0)
			ps_bump_index_version();

		/* Then the template's writes the frozen files lack */
		for (int i = 0; i < nsrc_top; i++)
			ps_snapshot_copy_fork(&src_top[i], dst_dbid);
		if (nsrc_top > 0)
		{
			char		dir[MAXPGPATH];

			ps_layer_path(dir, topgen);
			fsync_fname(dir, true);
		}
		pfree(src_top);

		LWLockRelease(&PsCtl->branch_lock);
	}
	PG_END_ENSURE_ERROR_CLEANUP(ps_snapshot_cleanup, PointerGetDatum(&state));

	pg_atomic_clear_flag(&PsCtl->reclaim_running);
	if (state.links)
		pfree(state.links);
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
				const uint32 *hidden, BlockNumber nblocks, bool yield,
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

		/*
		 * In pieces of at most 32MB, between which a clone waiting stops a
		 * reclaim in the background; the next one punches the rest.
		 */
		if (yield && ps_background_should_yield())
			break;
		for (end = start + 1;
			 end < nblocks && end - start < 4096 && PS_PUNCHABLE(end);
			 end++)
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
				PsDeadFork **dead, int *ndead, int *maxdead, bool yield,
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
								f->nblocks, yield, result);
			pfree(unwritten);
			pfree(deadbits);
		}

		/*
		 * A fork dropped over no files hides nothing, and neither is there a
		 * fork to read: the map saying it's dropped can go too.  Only once
		 * the files below are gone, which an earlier pass removes, so that a
		 * crash can't bring them back.
		 */
		if (f->kind == PS_FORK_DROPPED)
		{
			bool		below = false;

			for (int j = i - 1; j >= 0 && !below; j--)
				below = ps_frozen_lookup(layers[j], key) != NULL;
			if (!below)
				ps_add_dead_fork(dead, ndead, maxdead, layers[i], key);
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

		/* in the background, leave the rest for next time to a clone */
		if (!wait && ps_background_should_yield())
		{
			hash_seq_term(&status);
			break;
		}

		ps_reclaim_fork(layers, nlayers, key, &dead, &ndead, &maxdead, !wait,
						result);
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

/*
 * Forget everything this process read of frozen layers.  Reclaiming in
 * another process may have removed files since, and merging and reclaiming
 * need to know what's there; they exclude each other, so what they read
 * after this stays true while they run.
 */
static void
ps_forget_all_layers(void)
{
	HASH_SEQ_STATUS status;
	uint32	   *gen;
	uint32		gens[PAGESTORE_MAX_LAYERS * 2];
	int			ngens = 0;

	if (ps_indexed_gens == NULL)
		return;
	hash_seq_init(&status, ps_indexed_gens);
	while ((gen = hash_seq_search(&status)) != NULL)
	{
		if (ngens < lengthof(gens))
			gens[ngens++] = *gen;
	}
	for (int i = 0; i < ngens; i++)
		ps_forget_layer(gens[i]);
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
		ps_forget_all_layers();
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


/*
 * Merging layers
 *
 * Every pg_branch() after writes freezes a layer, and reading a block that
 * none of the upper layers hold looks at each one.  Merging two adjacent
 * frozen layers into one keeps the stack's depth bounded.  The merged layer
 * is a new layer, with a generation of its own, that resolves every block
 * as the pair did.  The pair's files stay as long as a branch links them.
 *
 * Most forks are in only one layer of the pair, and their files are linked
 * into the merged layer as they are, since frozen files never change.  Only
 * a fork both layers have is written anew: from a clone of the lower
 * layer's data file where the file system can clone (else a copy), with the
 * upper layer's blocks written over it.  Merging the adjacent pair whose
 * forks in common take the least space keeps that cheap; the bottom layer,
 * usually by far the largest, gets merged when nothing else will do.
 *
 * The merged layer replaces the pair in the stack under the branch lock,
 * which keeps pg_branch() from linking the pair meanwhile, and all partition
 * locks.  Then every backend is made to close its files, with the barrier
 * DROP DATABASE uses, before the pair's directories go: a storage manager
 * call resolves a block's layer and opens its file without processing the
 * barrier in between.
 *
 * Merging and reclaiming exclude each other: neither expects the other to
 * change frozen layers under it.
 */

/*
 * Clone a file where the file system can, else copy it, keeping holes.  If
 * yield is true, a copy stops early when a clone waits, setting *yielded.
 */
static void
ps_clone_file(const char *from, const char *to, bool yield, bool *yielded)
{
	char	   *buffer;
	int			srcfd;
	int			dstfd;
	off_t		offset = 0;
	struct stat st;

#if defined(__darwin__)
	if (clonefile(from, to, CLONE_NOFOLLOW) == 0)
		return;
	if (errno != ENOTSUP && errno != EXDEV)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not clone file \"%s\" to \"%s\": %m", from, to)));
#endif

	srcfd = OpenTransientFile(from, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", from)));
	dstfd = OpenTransientFile(to, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", to)));

#if defined(__linux__) && defined(FICLONE)
	if (ioctl(dstfd, FICLONE, srcfd) == 0)
	{
		CloseTransientFile(dstfd);
		CloseTransientFile(srcfd);
		return;
	}
#endif

	if (fstat(srcfd, &st) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", from)));

	buffer = palloc(BLCKSZ);
	for (;;)
	{
		ssize_t		nread;
		bool		zero = true;

		CHECK_FOR_INTERRUPTS();
		if (yield && (offset / BLCKSZ) % 1024 == 0 &&
			ps_background_should_yield())
		{
			*yielded = true;
			break;
		}
		nread = pg_pread(srcfd, buffer, BLCKSZ, offset);
		if (nread < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", from)));
		if (nread == 0)
			break;
		for (ssize_t i = 0; i < nread && zero; i++)
			zero = buffer[i] == 0;
		errno = 0;
		if (!zero && pg_pwrite(dstfd, buffer, nread, offset) != nread)
		{
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", to)));
		}
		offset += nread;
	}
	if (ftruncate(dstfd, st.st_size) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", to)));
	pfree(buffer);
	CloseTransientFile(dstfd);
	CloseTransientFile(srcfd);
}

static void
ps_link_file(const char *from, const char *to)
{
	if (link(from, to) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not link file \"%s\" to \"%s\": %m", from, to)));
}

/* Does the merge of the pair have to write this fork anew? */
static bool
ps_fork_needs_merging(const PsFrozenFork *lower, const PsFrozenFork *upper)
{
	return lower && upper &&
		lower->kind == PS_FORK_DATA && upper->kind == PS_FORK_DATA &&
		upper->base > 0;
}

static int64
ps_allocated_bytes(uint32 gen, const PsForkKey *key)
{
	char		path[MAXPGPATH];
	struct stat st;

	ps_data_path(path, gen, key);
	if (stat(path, &st) < 0)
		return 0;
	return (int64) st.st_blocks * 512;
}

/* The forks with files in either of two frozen layers */
static PsForkKey *
ps_pair_forks(uint32 lower, uint32 upper, int *nforks)
{
	HTAB	   *forks;
	HASHCTL		ctl;
	HASH_SEQ_STATUS status;
	PsFrozenFork *f;
	PsForkKey  *key;
	PsForkKey  *result;
	int			n = 0;

	ps_index_layer(lower);
	ps_index_layer(upper);

	ctl.keysize = sizeof(PsForkKey);
	ctl.entrysize = sizeof(PsForkKey);
	ctl.hcxt = CurrentMemoryContext;
	forks = hash_create("page store pair forks", 1024, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	hash_seq_init(&status, ps_frozen);
	while ((f = hash_seq_search(&status)) != NULL)
	{
		if (f->key.gen == lower || f->key.gen == upper)
			hash_search(forks, &f->key.fork, HASH_ENTER, NULL);
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
 * What merging a pair would write: the forks both layers have.  If yield is
 * true, gives up as soon as a clone waits, setting *yielded.
 */
static int64
ps_pair_cost(uint32 lower, uint32 upper, bool yield, bool *yielded)
{
	PsForkKey  *forks;
	int			nforks;
	int64		cost = 0;

	forks = ps_pair_forks(lower, upper, &nforks);
	for (int i = 0; i < nforks; i++)
	{
		if (yield && ps_background_should_yield())
		{
			*yielded = true;
			break;
		}
		if (ps_fork_needs_merging(ps_frozen_lookup(lower, &forks[i]),
								  ps_frozen_lookup(upper, &forks[i])))
			cost += ps_allocated_bytes(lower, &forks[i]) +
				ps_allocated_bytes(upper, &forks[i]);
	}
	pfree(forks);
	return cost;
}

/*
 * Write a fork both layers of the pair have into the merged layer: the
 * lower layer's data file, with the blocks the upper layer holds written
 * over it, cut to the upper layer's size.
 */
static int64
ps_merge_fork(uint32 newgen, const PsForkKey *key, PsFrozenFork *lower,
			  PsFrozenFork *upper, bool yield, bool *yielded)
{
	char		path[MAXPGPATH];
	char		srcpath[MAXPGPATH];
	PGIOAlignedBlock buf;
	int			fd;
	int			srcfd = -1;
	int64		written = 0;
	BlockNumber base;
	uint32	   *bits;
	uint32		checked = 0;

	ps_data_path(path, newgen, key);
	if (lower->has_data)
	{
		ps_data_path(srcpath, lower->key.gen, key);
		ps_clone_file(srcpath, path, yield, yielded);
		if (*yielded)
			return written;
	}
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	if (ftruncate(fd, (off_t) upper->nblocks * BLCKSZ) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", path)));

	if (upper->has_data)
	{
		ps_data_path(srcpath, upper->key.gen, key);
		srcfd = OpenTransientFile(srcpath, O_RDONLY | PG_BINARY);
		if (srcfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", srcpath)));
	}

	for (BlockNumber blkno = 0; blkno < upper->nblocks; blkno++)
	{
		ssize_t		nread = 0;
		bool		zero = true;

		if (!ps_frozen_holds(upper, blkno))
			continue;
		CHECK_FOR_INTERRUPTS();
		if (yield && ++checked % 1024 == 0 &&
			ps_background_should_yield())
		{
			*yielded = true;
			break;
		}

		if (srcfd >= 0)
		{
			nread = pg_pread(srcfd, buf.data, BLCKSZ, (off_t) blkno * BLCKSZ);
			if (nread < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m", srcpath)));
		}
		/* past the end of the upper layer's file, the block is zeroes */
		if (nread < BLCKSZ)
			memset(buf.data + Max(nread, 0), 0, BLCKSZ - Max(nread, 0));
		for (ssize_t i = 0; i < nread && zero; i++)
			zero = buf.data[i] == 0;

		/* the upper layer's block, a hole if it has none */
		if (zero)
		{
			if (ps_punch_hole(fd, (off_t) blkno * BLCKSZ, BLCKSZ) < 0)
			{
				memset(buf.data, 0, BLCKSZ);
				zero = false;
			}
		}
		if (!zero)
		{
			if (pg_pwrite(fd, buf.data, BLCKSZ, (off_t) blkno * BLCKSZ) != BLCKSZ)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write file \"%s\": %m", path)));
			written += BLCKSZ;
		}
	}

	/* what's written is thrown away with the merged layer */
	if (*yielded)
	{
		CloseTransientFile(fd);
		if (srcfd >= 0)
			CloseTransientFile(srcfd);
		return written;
	}

	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	CloseTransientFile(fd);
	if (srcfd >= 0)
		CloseTransientFile(srcfd);

	/*
	 * The pair holds a block if either layer does: from the smaller base on,
	 * and below it the blocks either bitmap has.  A data file without a map
	 * has base 0.
	 */
	base = Min(lower->base, upper->base);
	if (base > 0)
	{
		bits = palloc0(ps_bitmap_words(base) * sizeof(uint32));
		for (size_t w = 0; w < ps_bitmap_words(base); w++)
		{
			if (lower->bits && w < ps_bitmap_words(lower->base))
				bits[w] |= lower->bits[w];
			if (upper->bits && w < ps_bitmap_words(upper->base))
				bits[w] |= upper->bits[w];
		}
		ps_write_map(newgen, key, 0, base, bits, false, ERROR);
		pfree(bits);
	}

	return written;
}

/*
 * The files a fork both layers of a pair have, by identity.  Databases that
 * CREATE DATABASE ... STRATEGY SNAPSHOT made share files, under their own
 * names; forks with the same files merge the same, so the merge is done once
 * and its result given each of their names.
 */
typedef struct PsPairFiles
{
	ino_t		ino[4];			/* lower data, lower map, upper data, upper
								 * map; 0 for none */
} PsPairFiles;

typedef struct PsMergedFork
{
	PsPairFiles files;			/* hash key, must be first */
	PsForkKey	key;			/* the fork whose merge wrote the result */
} PsMergedFork;

static void
ps_pair_files(uint32 lower, uint32 upper, const PsForkKey *key,
			  PsPairFiles *files)
{
	memset(files, 0, sizeof(PsPairFiles));
	for (int i = 0; i < 4; i++)
	{
		char		path[MAXPGPATH];
		struct stat st;

		ps_name_path(path, i < 2 ? lower : upper, key, i % 2 == 1);
		if (stat(path, &st) == 0)
			files->ino[i] = st.st_ino;
		else if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", path)));
	}
}

/* Link a file of the merged layer, if it has one. */
static void
ps_link_merged(uint32 newgen, const PsForkKey *from_key,
			   const PsForkKey *to_key, bool is_map)
{
	char		from[MAXPGPATH];
	char		to[MAXPGPATH];

	ps_name_path(from, newgen, from_key, is_map);
	ps_name_path(to, newgen, to_key, is_map);
	if (link(from, to) < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not link file \"%s\" to \"%s\": %m", from, to)));
}

/*
 * Make the layer that resolves every block as the pair does.  If yield is
 * true, gives up as soon as a clone waits, setting *yielded.
 */
static int64
ps_merge_pair(uint32 lower, uint32 upper, uint32 newgen, bool yield,
			  bool *yielded)
{
	char		dir[MAXPGPATH];
	PsForkKey  *forks;
	int			nforks;
	int64		written = 0;
	HTAB	   *merged;
	HASHCTL		ctl;

	ps_layer_path(dir, newgen);
	if (MakePGDirectory(dir) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir)));

	fsync_fname(PAGESTORE_DIR, true);

	ctl.keysize = sizeof(PsPairFiles);
	ctl.entrysize = sizeof(PsMergedFork);
	ctl.hcxt = CurrentMemoryContext;
	merged = hash_create("page store merged forks", 64, &ctl,
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	forks = ps_pair_forks(lower, upper, &nforks);
	for (int i = 0; i < nforks; i++)
	{
		PsFrozenFork *fl = ps_frozen_lookup(lower, &forks[i]);
		PsFrozenFork *fu = ps_frozen_lookup(upper, &forks[i]);
		PsFrozenFork *src;
		char		from[MAXPGPATH];
		char		to[MAXPGPATH];

		CHECK_FOR_INTERRUPTS();
		if (yield && ps_background_should_yield())
		{
			*yielded = true;
			break;
		}

		if (ps_fork_needs_merging(fl, fu))
		{
			PsPairFiles files;
			PsMergedFork *m;
			bool		found;

			ps_pair_files(lower, upper, &forks[i], &files);
			m = hash_search(merged, &files, HASH_ENTER, &found);
			if (found)
			{
				ps_link_merged(newgen, &m->key, &forks[i], true);
				ps_link_merged(newgen, &m->key, &forks[i], false);
				continue;
			}
			m->key = forks[i];
			written += ps_merge_fork(newgen, &forks[i], fl, fu, yield, yielded);
			if (*yielded)
				break;
			continue;
		}

		/*
		 * One layer decides alone: it's the only one with the fork, or the
		 * upper layer hides all of the lower one's.
		 */
		if (fu == NULL)
			src = fl;
		else if (fl == NULL || fu->kind == PS_FORK_DROPPED || fu->base == 0)
			src = fu;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("page store layer %u inherits blocks of a fork that layer %u dropped",
							upper, lower)));
		if (src == NULL)
			continue;

		if (src->has_data)
		{
			ps_data_path(from, src->key.gen, &forks[i]);
			ps_data_path(to, newgen, &forks[i]);
			ps_link_file(from, to);
		}
		if (src->has_map)
		{
			ps_map_path(from, src->key.gen, &forks[i]);
			ps_map_path(to, newgen, &forks[i]);
			ps_link_file(from, to);
		}
	}
	pfree(forks);
	hash_destroy(merged);

	if (!*yielded)
		fsync_fname(dir, true);
	return written;
}

/*
 * Layers taken off the stack are renamed "<gen>.trash", which is quick, and
 * removed later, once the merge has let go of reclaim_running: removing
 * gigabytes can take long, and a clone may be waiting.  Nothing reads them by
 * then.  What a crash leaves, the startup removes with the other layers not
 * in the stack.
 */
static void
ps_trash_layer(uint32 gen)
{
	char		dir[MAXPGPATH];
	char		trash[MAXPGPATH];

	ps_layer_path(dir, gen);
	snprintf(trash, sizeof(trash), "%s.trash", dir);
	if (rename(dir, trash) < 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not rename directory \"%s\" to \"%s\": %m",
						dir, trash)));
}

static void
ps_empty_trash(void)
{
	DIR		   *dirdesc;
	struct dirent *de;
	bool		removed = false;

	/* one process at a time, or they trip over each other's removals */
	if (!pg_atomic_test_set_flag(&PsCtl->trash_emptying))
		return;

	dirdesc = AllocateDir(PAGESTORE_DIR);
	while ((de = ReadDir(dirdesc, PAGESTORE_DIR)) != NULL)
	{
		size_t		len = strlen(de->d_name);
		char		path[MAXPGPATH];

		if (len <= 6 || strcmp(de->d_name + len - 6, ".trash") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", PAGESTORE_DIR, de->d_name);
		if (!rmtree(path, true))
			ereport(WARNING,
					(errmsg("could not remove directory \"%s\"", path)));
		removed = true;
	}
	FreeDir(dirdesc);
	if (removed)
		fsync_fname(PAGESTORE_DIR, true);

	pg_atomic_clear_flag(&PsCtl->trash_emptying);
}

/*
 * Find layer directories that the stack doesn't list, while the server runs:
 * what a merge that failed after putting its merged layer in the stack left,
 * or one that failed or gave way to a clone before.  Found under the branch
 * lock, so no freeze is making a directory for a layer not yet in the stack,
 * and with reclaim_running held, so no merge is making one.  Generations
 * never repeat, so nothing new can take their names.  Adds them to gone, to
 * be removed once every backend has closed its files: see ps_remove_layers().
 */
static void
ps_find_unlisted_layers(bool wait, uint32 *gone, int *ngone)
{
	DIR		   *dirdesc;
	struct dirent *de;

	if (wait)
		LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE))
		return;

	dirdesc = AllocateDir(PAGESTORE_DIR);
	while ((de = ReadDir(dirdesc, PAGESTORE_DIR)) != NULL &&
		   *ngone < PAGESTORE_MAX_LAYERS)
	{
		char	   *end;
		unsigned long gen;
		bool		listed = false;

		gen = strtoul(de->d_name, &end, 10);
		if (end == de->d_name || *end != '\0' || gen == 0 ||
			gen > PG_UINT32_MAX)
			continue;
		LWLockAcquire(&PsCtl->partition_locks[0].lock, LW_SHARED);
		for (int i = 0; i < PsCtl->nlayers; i++)
			listed |= PsCtl->layers[i] == gen;
		LWLockRelease(&PsCtl->partition_locks[0].lock);
		if (!listed)
			gone[(*ngone)++] = (uint32) gen;
	}
	FreeDir(dirdesc);
	LWLockRelease(&PsCtl->branch_lock);
}

/*
 * Remove layers taken off the stack, once every backend has closed its files
 * of them.  Called without reclaim_running: the wait for the backends can
 * take as long as a checkpoint, which the checkpointer finishes before
 * closing its files, and a clone may be waiting for the flag.
 */
static void
ps_remove_layers(const uint32 *gone, int ngone)
{
	if (ngone > 0)
	{
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));
		for (int i = 0; i < ngone; i++)
			ps_trash_layer(gone[i]);
		fsync_fname(PAGESTORE_DIR, true);
	}
	ps_empty_trash();
}

/*
 * Merge one pair of adjacent frozen layers if there are more than
 * max_frozen of them.  Returns false if there were not.
 */
static bool
ps_merge_once(int max_frozen, bool wait, PageStoreMergeResult *result,
			  bool *yielded, uint32 *gone, int *ngone)
{
	uint32		layers[PAGESTORE_MAX_LAYERS];
	int			nlayers;
	int			best = -1;
	int64		best_cost = PG_INT64_MAX;
	uint32		newgen;
	bool		swapped = false;

	/* The frozen layers, which are durable once pg_branch() is done */
	if (wait)
		LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE))
		return false;
	ps_acquire_all_partitions(LW_EXCLUSIVE);
	ps_sync_stack();
	nlayers = ps_local_nlayers - 1;
	memcpy(layers, ps_local_layers, sizeof(uint32) * nlayers);
	newgen = nlayers > max_frozen ? PsCtl->next_gen++ : 0;
	ps_release_all_partitions();
	LWLockRelease(&PsCtl->branch_lock);

	if (nlayers <= max_frozen)
		return false;

	for (int i = 0; i + 1 < nlayers; i++)
	{
		int64		cost = ps_pair_cost(layers[i], layers[i + 1], !wait,
										yielded);

		if (*yielded)
			return false;
		if (cost < best_cost)
		{
			best = i;
			best_cost = cost;
		}
	}

	result->written_bytes += ps_merge_pair(layers[best], layers[best + 1],
										   newgen, !wait, yielded);

	/*
	 * A clone waits: leave the pair for next time, and the merged layer,
	 * which isn't in the stack, for the next merge to remove.  Removing it
	 * now could keep the clone waiting.
	 */
	if (*yielded)
		return false;

	/* Put the merged layer in the pair's place */
	LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	ps_acquire_all_partitions(LW_EXCLUSIVE);
	for (int i = 0; i + 2 < PsCtl->nlayers; i++)
	{
		uint32		newlayers[PAGESTORE_MAX_LAYERS];
		int			nnew = 0;

		if (PsCtl->layers[i] != layers[best] ||
			PsCtl->layers[i + 1] != layers[best + 1])
			continue;

		memcpy(newlayers, PsCtl->layers, sizeof(uint32) * i);
		nnew = i;
		newlayers[nnew++] = newgen;
		for (int j = i + 2; j < PsCtl->nlayers; j++)
			newlayers[nnew++] = PsCtl->layers[j];
		PageStoreWriteStackFile(NULL, newlayers, nnew);
		memcpy(PsCtl->layers, newlayers, sizeof(uint32) * nnew);
		PsCtl->nlayers = nnew;
		PsCtl->stack_version++;
		swapped = true;
		break;
	}
	ps_release_all_partitions();
	LWLockRelease(&PsCtl->branch_lock);

	if (!swapped)
		elog(ERROR, "page store layers %u and %u are no longer adjacent",
			 layers[best], layers[best + 1]);

	/* Removed once no backend reads the pair any more, see ps_remove_layers() */
	for (int i = best; i <= best + 1; i++)
	{
		if (*ngone < PAGESTORE_MAX_LAYERS * 2)
			gone[(*ngone)++] = layers[i];
		ps_forget_layer(layers[i]);
	}

	result->merged_layers++;
	return true;
}

/*
 * Merge adjacent frozen layers until there are at most max_frozen.  If wait
 * is false, return false rather than wait for pg_branch() or for another
 * process merging or reclaiming.
 */
bool
PageStoreMerge(int max_frozen, bool wait, PageStoreMergeResult *result)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	bool		yielded = false;
	uint32		gone[PAGESTORE_MAX_LAYERS * 2];
	int			ngone = 0;

	memset(result, 0, sizeof(PageStoreMergeResult));

	if (!PageStoreEnabled())
		return true;
	while (!pg_atomic_test_set_flag(&PsCtl->reclaim_running))
	{
		if (!wait)
			return false;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(10000L);
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext, "page store merge",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	PG_ENSURE_ERROR_CLEANUP(ps_reclaim_cleanup, (Datum) 0);
	{
		ps_forget_all_layers();
		ps_find_unlisted_layers(wait, gone, &ngone);
		while (ps_merge_once(Max(max_frozen, 1), wait, result, &yielded,
							 gone, &ngone))
			MemoryContextReset(cxt);
	}
	PG_END_ENSURE_ERROR_CLEANUP(ps_reclaim_cleanup, (Datum) 0);
	pg_atomic_clear_flag(&PsCtl->reclaim_running);

	ps_remove_layers(gone, ngone);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
	return true;
}

/*
 * The page store merger: a background worker that keeps the number of frozen
 * layers at pagestore_max_frozen_layers.
 */
void
PageStoreMergerRegister(void)
{
	BackgroundWorker bgw;

	if (!ps_dir_exists() || IsBinaryUpgrade)
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "PageStoreMergerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "page store merger");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "page store merger");
	bgw.bgw_restart_time = 10;

	RegisterBackgroundWorker(&bgw);
}

/*
 * Freeze the top layer if its tables are getting full, as
 * ps_maybe_request_freeze() asked.  The page store merger's job.
 */
static void
ps_auto_freeze(void)
{
	uint32		layers[PAGESTORE_MAX_LAYERS];
	XLogRecPtr	insert_end;
	uint32		forks;
	uint32		chunks;

	if (pg_atomic_unlocked_test_flag(&PsCtl->freeze_requested))
		return;
	if (!pagestore_auto_freeze)
	{
		pg_atomic_clear_flag(&PsCtl->freeze_requested);
		return;
	}

	forks = pg_atomic_read_u32(&PsCtl->top_forks);
	chunks = pg_atomic_read_u32(&PsCtl->top_chunks);
	LWLockAcquire(&PsCtl->branch_lock, LW_EXCLUSIVE);
	(void) PageStoreFreeze(layers, &insert_end);
	LWLockRelease(&PsCtl->branch_lock);
	pg_atomic_clear_flag(&PsCtl->freeze_requested);

	ereport(LOG,
			(errmsg("page store froze its top layer, which held %u of %d fork entries and %u of %d bitmap chunks",
					forks, pagestore_max_forks,
					chunks, pagestore_max_bitmap_chunks)));
}

void
PageStoreMergerMain(Datum main_arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	/* where ps_maybe_request_freeze() wakes us */
	PsCtl->merger_procno = MyProcNumber;

	for (;;)
	{
		PageStoreMergeResult result;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		ps_auto_freeze();
		if (pagestore_max_frozen_layers > 0 &&
			PageStoreMerge(pagestore_max_frozen_layers, false, &result) &&
			result.merged_layers > 0)
			ereport(LOG,
					(errmsg("page store merged %d layers, writing %lld kB",
							result.merged_layers * 2,
							(long long) (result.written_bytes / 1024))));
		/* a merge stops early when asked to freeze */
		ps_auto_freeze();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10000L, WAIT_EVENT_PAGESTORE_MERGER_MAIN);
		ResetLatch(MyLatch);
	}
}
