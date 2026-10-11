/*-------------------------------------------------------------------------
 *
 * pagestore.h
 *	  Copy-on-write storage manager, and branching of a cluster built on it.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pagestore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGESTORE_H
#define PAGESTORE_H

#include "access/xlogdefs.h"
#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/lwlock.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"

/*
 * A cluster uses the page store if its data directory has this directory,
 * created by initdb --pagestore.  It holds one directory per layer, named by
 * the layer's generation number, and the STACK file listing the layers'
 * generations from the bottom of the stack to the top.
 */
#define PAGESTORE_DIR			"pg_pagestore"
#define PAGESTORE_STACK_FILE	PAGESTORE_DIR "/STACK"

/* Most layers a stack can have */
#define PAGESTORE_MAX_LAYERS	256

/* GUCs */
extern PGDLLIMPORT int pagestore_max_forks;
extern PGDLLIMPORT int pagestore_max_bitmap_chunks;
extern PGDLLIMPORT bool pagestore_reclaim;
extern PGDLLIMPORT int pagestore_max_frozen_layers;

/* What PageStoreReclaim() gave back */
typedef struct PageStoreReclaimResult
{
	int64		removed_files;	/* files unlinked from frozen layers */
	int			removed_layers; /* empty layers taken off the stack */
	int64		reclaimed_bytes;	/* disk space no longer used by them */
} PageStoreReclaimResult;

extern bool PageStoreEnabled(void);
extern bool PageStoreHandlesRelation(RelFileLocator rlocator, ProcNumber backend);

/* page store storage manager functionality */
extern void psinit(void);
extern void psopen(SMgrRelation reln);
extern void psclose(SMgrRelation reln, ForkNumber forknum);
extern void pscreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool psexists(SMgrRelation reln, ForkNumber forknum);
extern void psunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo);
extern void psextend(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, const void *buffer, bool skipFsync);
extern void pszeroextend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks, bool skipFsync);
extern bool psprefetch(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, int nblocks);
extern uint32 psmaxcombine(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum);
extern void psreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					void **buffers, BlockNumber nblocks);
extern void psstartreadv(PgAioHandle *ioh,
						 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
						 void **buffers, BlockNumber nblocks);
extern void pswritev(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum,
					 const void **buffers, BlockNumber nblocks, bool skipFsync);
extern void pswriteback(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber psnblocks(SMgrRelation reln, ForkNumber forknum);
extern void pstruncate(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber curnblk, BlockNumber nblocks);
extern void psimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void psregistersync(SMgrRelation reln, ForkNumber forknum);
extern int	psfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint64 *off);

/* page store sync callbacks */
extern int	pssyncfiletag(const FileTag *ftag, char *path);
extern int	psunlinkfiletag(const FileTag *ftag, char *path);
extern bool psfiletagmatches(const FileTag *ftag, const FileTag *candidate);

/* operations on all of a database's relations */
extern void PageStoreForgetDatabaseSyncRequests(Oid dbid);
extern void PageStoreCheckPoint(void);
extern void PageStoreDropDatabase(Oid dbid);
extern void PageStoreCopyDatabase(Oid src_dbid, Oid dst_dbid, Oid dst_spcoid,
								  bool isRedo);
extern void PageStoreResetUnloggedRelations(int op);
extern int64 PageStoreForkSize(RelFileLocator rlocator, ForkNumber forknum);
extern int64 PageStoreDatabaseSize(Oid dbid);
extern bool PageStoreRelFileNumberInUse(RelFileLocator rlocator);

/* support for pg_branch() */
extern int	PageStoreFreeze(uint32 *layers, XLogRecPtr *insert_end);
extern void PageStoreWriteStackFile(const char *dir, const uint32 *layers,
									int nlayers);
extern LWLock *PageStoreBranchLock(void);
extern void PageStoreSetBranchKeepLSN(XLogRecPtr lsn);
extern XLogRecPtr PageStoreGetBranchKeepLSN(void);

/* reclaiming space of frozen layers */
extern bool PageStoreReclaim(PageStoreReclaimResult *result, bool wait);
extern void PageStoreReclaimAfterCheckpoint(void);

/* What PageStoreMerge() did */
typedef struct PageStoreMergeResult
{
	int			merged_layers;	/* pairs of layers merged into one */
	int64		written_bytes;	/* data written for forks both had */
} PageStoreMergeResult;

/* merging frozen layers */
extern bool PageStoreMerge(int max_frozen, bool wait,
						   PageStoreMergeResult *result);
extern void PageStoreMergerRegister(void);
pg_noreturn extern void PageStoreMergerMain(Datum main_arg);

#endif							/* PAGESTORE_H */
