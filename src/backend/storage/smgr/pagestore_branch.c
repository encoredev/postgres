/*-------------------------------------------------------------------------
 *
 * pagestore_branch.c
 *	  pg_branch(): copy a running cluster into a new data directory.
 *
 * The copy is consistent as of the WAL position pg_branch() returns, and is
 * made without disturbing the source's sessions.  The new data directory
 * looks like that of a server that crashed at that position: its control
 * file points at the source's last checkpoint, and its pg_wal holds the WAL
 * from that checkpoint's redo point up to the position.  Starting a server
 * on it runs crash recovery, which makes it consistent.
 *
 * Relation data isn't copied, which is what makes this cheap.  The source's
 * page store top layer is frozen, and every frozen layer is hard-linked into
 * the new data directory, which gets an empty top layer of its own.  What
 * is copied is small: the transaction status SLRUs, configuration files and
 * the like, and the WAL written since the last checkpoint.
 *
 * The branch replays the WAL from the last checkpoint, so by default
 * pg_branch() first asks for a checkpoint.  That writes out at most
 * shared_buffers of dirty pages, while the WAL since the last checkpoint
 * can be as much as max_wal_size.
 *
 * Correctness rests on the order of the steps:
 *
 * 1. Note the last checkpoint and keep the WAL from its redo point.  Every
 *	  page write the checkpoint relied on happened before it completed, so
 *	  it is in the layers frozen in step 3.
 * 2. Copy the files outside the page store.  Whatever they hold was
 *	  WAL-logged first, or is rebuilt by recovery, by WAL inserted before
 *	  step 4 reads the insert position.
 * 3. Freeze the page store.  Every page in the frozen layers was written
 *	  after the WAL describing it was flushed.  No page is torn: the freeze
 *	  waits for writes in progress.
 * 4. While the freeze holds off writes, take the WAL insert position as the
 *	  branch point.  WAL logged after a write it depends on, like the commit
 *	  of a relation created without WAL-logging its contents, then falls
 *	  after the branch point unless the write is in the frozen layers.
 *	  Flush WAL up to the branch point and copy the WAL from the redo point
 *	  to there and no further.
 *
 * Recovery in the branch replays from the redo point to the branch point
 * over pages that are each at some state between the two, restoring every
 * page modified after the redo point from its first full-page image, which
 * is the crash recovery case.  Transactions that hadn't committed at the
 * branch point are rolled back in the branch.
 *
 * The maps that freezing writes into the frozen layer aren't fsync'd first,
 * so if the operating system crashes before the next checkpoint, a map may
 * survive while a block it points to doesn't.  Like a torn page, that block
 * is restored by its full-page image, provided changes to hint bits are
 * WAL-logged too: pg_branch() requires data checksums or wal_log_hints.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/pagestore_branch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__darwin__)
#include <sys/clonefile.h>
#endif

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "common/file_utils.h"
#include "common/relpath.h"
#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "port/pg_crc32c.h"
#include "portability/instr_time.h"
#include "postmaster/bgwriter.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pagestore.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

/* Directories whose contents a branch doesn't need; it gets them empty */
static const char *const empty_dirs[] = {
	"pg_dynshmem",
	"pg_notify",
	"pg_replslot",
	"pg_serial",
	"pg_snapshots",
	"pg_stat",
	"pg_stat_tmp",
	"pg_subtrans",
	"pg_tblspc",
	NULL
};

/* Files in the top directory a branch must not have */
static const char *const skip_files[] = {
	"postmaster.pid",
	"postmaster.opts",
	"current_logfiles",
	BACKUP_LABEL_FILE,
	TABLESPACE_MAP,
	RECOVERY_SIGNAL_FILE,
	STANDBY_SIGNAL_FILE,
	NULL
};

static bool
name_in(const char *name, const char *const *names)
{
	for (int i = 0; names[i] != NULL; i++)
	{
		if (strcmp(name, names[i]) == 0)
			return true;
	}
	return false;
}

static void
branch_mkdir(const char *path)
{
	if (MakePGDirectory(path) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", path)));
}

/*
 * Copy at most len bytes of a file (all of it if len is negative), and
 * fsync the copy.  If the file is gone, as files in pg_logical can be by
 * now, skip it and return false.
 */
static bool
branch_copy_file(const char *from, const char *to, pgoff_t len)
{
	char	   *buffer = palloc(8 * BLCKSZ);
	int			srcfd;
	int			dstfd;
	pgoff_t		copied = 0;

	srcfd = OpenTransientFile(from, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
	{
		if (errno == ENOENT)
		{
			pfree(buffer);
			return false;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", from)));
	}

	dstfd = OpenTransientFile(to, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", to)));

	while (len < 0 || copied < len)
	{
		size_t		want = 8 * BLCKSZ;
		ssize_t		nread;

		if (len >= 0 && len - copied < (pgoff_t) want)
			want = len - copied;
		nread = read(srcfd, buffer, want);
		if (nread < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", from)));
		if (nread == 0)
			break;
		errno = 0;
		if (write(dstfd, buffer, nread) != nread)
		{
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", to)));
		}
		copied += nread;
	}

	if (pg_fsync(dstfd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", to)));
	if (CloseTransientFile(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", to)));
	CloseTransientFile(srcfd);
	pfree(buffer);
	return true;
}

/*
 * Copy a directory of the data directory, apart from the page store, the
 * WAL and what a server rebuilds or must not inherit.
 */
static void
branch_copy_dir(const char *from, const char *to, bool toplevel)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(from);
	while ((de = ReadDir(dir, from)) != NULL)
	{
		char		srcpath[MAXPGPATH];
		char		dstpath[MAXPGPATH];
		PGFileType	type;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* relcache init files, temporary files and temporary relations */
		if (strncmp(de->d_name, "pg_internal.init", 16) == 0 ||
			strncmp(de->d_name, PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0 ||
			looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(srcpath, sizeof(srcpath), "%s/%s", from, de->d_name);
		snprintf(dstpath, sizeof(dstpath), "%s/%s", to, de->d_name);

		if (toplevel)
		{
			if (strcmp(de->d_name, "pg_wal") == 0 ||
				strcmp(de->d_name, PAGESTORE_DIR) == 0 ||
				name_in(de->d_name, skip_files))
				continue;
			if (name_in(de->d_name, empty_dirs) ||
				strcmp(de->d_name, Log_directory) == 0)
			{
				branch_mkdir(dstpath);
				continue;
			}
		}

		type = get_dirent_type(srcpath, de, false, ERROR);
		if (type == PGFILETYPE_DIR)
		{
			branch_mkdir(dstpath);
			branch_copy_dir(srcpath, dstpath, false);
		}
		else if (type == PGFILETYPE_REG)
		{
			/* written from the checkpoint we start from, at the end */
			if (strcmp(srcpath, "./" XLOG_CONTROL_FILE) == 0)
				continue;
			branch_copy_file(srcpath, dstpath, -1);
		}
	}
	FreeDir(dir);

	fsync_fname(to, true);
}

/*
 * Hard-link the files of a layer directory into a new one.
 */
static void
branch_link_dir(const char *from, const char *to)
{
	DIR		   *dir;
	struct dirent *de;

	branch_mkdir(to);

	dir = AllocateDir(from);
	while ((de = ReadDir(dir, from)) != NULL)
	{
		char		srcpath[MAXPGPATH];
		char		dstpath[MAXPGPATH];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(srcpath, sizeof(srcpath), "%s/%s", from, de->d_name);
		snprintf(dstpath, sizeof(dstpath), "%s/%s", to, de->d_name);
		if (link(srcpath, dstpath) < 0)
		{
			bool		exdev = errno == EXDEV;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not link file \"%s\" to \"%s\": %m",
							srcpath, dstpath),
					 exdev ?
					 errhint("The target directory must be on the same file system as the data directory.") : 0));
		}
	}
	FreeDir(dir);
}

/*
 * Link the frozen layers into the branch, and give it an empty top layer.
 *
 * A frozen layer's files never change, so a copy-on-write clone shares them
 * as well as a hard link does.  Where a whole directory can be cloned at
 * once, as on APFS, that is much faster than linking its files one by one.
 */
static void
branch_link_layers(const char *target, const uint32 *layers, int nlayers)
{
	char		path[MAXPGPATH];
	uint32		stack[PAGESTORE_MAX_LAYERS];

	snprintf(path, sizeof(path), "%s/%s", target, PAGESTORE_DIR);
	branch_mkdir(path);

	for (int i = 0; i < nlayers; i++)
	{
		char		from[MAXPGPATH];
		char		to[MAXPGPATH];

		snprintf(from, sizeof(from), "%s/%u", PAGESTORE_DIR, layers[i]);
		snprintf(to, sizeof(to), "%s/%s/%u", target, PAGESTORE_DIR, layers[i]);

#if defined(__darwin__)
		if (clonefile(from, to, CLONE_NOFOLLOW) < 0)
		{
			if (errno != ENOTSUP && errno != EXDEV)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not clone directory \"%s\" to \"%s\": %m",
								from, to)));
			branch_link_dir(from, to);
		}
#else
		branch_link_dir(from, to);
#endif
		fsync_fname(to, true);

		stack[i] = layers[i];
	}

	/* the branch writes into a layer of its own */
	stack[nlayers] = nlayers > 0 ? layers[nlayers - 1] + 1 : 1;
	snprintf(path, sizeof(path), "%s/%s/%u", target, PAGESTORE_DIR,
			 stack[nlayers]);
	branch_mkdir(path);
	fsync_fname(path, true);

	PageStoreWriteStackFile(target, stack, nlayers + 1);
}

/*
 * Copy the WAL from start up to end into the branch.  The rest of the last
 * segment is zeroes, so recovery stops at end.
 */
static void
branch_copy_wal(const char *target, TimeLineID tli, XLogRecPtr start,
				XLogRecPtr end)
{
	char		path[MAXPGPATH];
	XLogSegNo	startseg;
	XLogSegNo	endseg;
	DIR		   *dir;
	struct dirent *de;

	snprintf(path, sizeof(path), "%s/%s", target, XLOGDIR);
	branch_mkdir(path);
	snprintf(path, sizeof(path), "%s/%s/archive_status", target, XLOGDIR);
	branch_mkdir(path);
	snprintf(path, sizeof(path), "%s/%s/summaries", target, XLOGDIR);
	branch_mkdir(path);

	XLByteToSeg(start, startseg, wal_segment_size);
	XLByteToPrevSeg(end, endseg, wal_segment_size);

	for (XLogSegNo segno = startseg; segno <= endseg; segno++)
	{
		char		fname[MAXFNAMELEN];
		char		from[MAXPGPATH];
		XLogRecPtr	segstart;
		int			fd;

		XLogFileName(fname, tli, segno, wal_segment_size);
		snprintf(from, sizeof(from), "%s/%s", XLOGDIR, fname);
		snprintf(path, sizeof(path), "%s/%s/%s", target, XLOGDIR, fname);
		XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, segstart);

		if (!branch_copy_file(from, path,
							  Min(end - segstart, (uint64) wal_segment_size)))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", from)));

		fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
		if (fd < 0 || ftruncate(fd, wal_segment_size) < 0 || pg_fsync(fd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m", path)));
		CloseTransientFile(fd);
	}

	/* timeline history, in case recovery wants it */
	dir = AllocateDir(XLOGDIR);
	while ((de = ReadDir(dir, XLOGDIR)) != NULL)
	{
		char		from[MAXPGPATH];

		if (!IsTLHistoryFileName(de->d_name))
			continue;
		snprintf(from, sizeof(from), "%s/%s", XLOGDIR, de->d_name);
		snprintf(path, sizeof(path), "%s/%s/%s", target, XLOGDIR, de->d_name);
		branch_copy_file(from, path, -1);
	}
	FreeDir(dir);

	snprintf(path, sizeof(path), "%s/%s", target, XLOGDIR);
	fsync_fname(path, true);
}

/*
 * Turn off archiving in the branch, by appending to its
 * postgresql.auto.conf, which ALTER SYSTEM rewrites as needed.
 */
static void
branch_disable_archiving(const char *target)
{
	char		path[MAXPGPATH];
	const char *setting = "# added by pg_branch()\narchive_mode = 'off'\n";
	int			fd;

	snprintf(path, sizeof(path), "%s/%s", target, PG_AUTOCONF_FILENAME);
	fd = OpenTransientFile(path, O_WRONLY | O_CREAT | O_APPEND | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	errno = 0;
	if (write(fd, setting, strlen(setting)) != strlen(setting))
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	CloseTransientFile(fd);
}

/*
 * Write the branch's control file.  Not with update_controlfile(), whose
 * failures are PANICs: it's meant for our own control file.
 */
static void
branch_write_control_file(const char *target, ControlFileData *cf)
{
	char		buffer[PG_CONTROL_FILE_SIZE];
	char		path[MAXPGPATH];
	int			fd;

	cf->time = (pg_time_t) time(NULL);
	INIT_CRC32C(cf->crc);
	COMP_CRC32C(cf->crc, cf, offsetof(ControlFileData, crc));
	FIN_CRC32C(cf->crc);

	memset(buffer, 0, PG_CONTROL_FILE_SIZE);
	memcpy(buffer, cf, sizeof(ControlFileData));

	snprintf(path, sizeof(path), "%s/%s", target, XLOG_CONTROL_FILE);
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));
	errno = 0;
	if (write(fd, buffer, PG_CONTROL_FILE_SIZE) != PG_CONTROL_FILE_SIZE)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	snprintf(path, sizeof(path), "%s/global", target);
	fsync_fname(path, true);
}

/* On failure, stop keeping WAL for the branch and remove what was made */
static void
branch_failed(int code, Datum arg)
{
	const char *target = DatumGetCString(arg);

	PageStoreSetBranchKeepLSN(InvalidXLogRecPtr);
	if (!rmtree(target, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", target)));
}

/* Log how long a step took, for anyone wondering where the time goes */
static void
branch_step_done(const char *step, instr_time *start)
{
	instr_time	now;

	INSTR_TIME_SET_CURRENT(now);
	INSTR_TIME_SUBTRACT(now, *start);
	elog(DEBUG1, "pg_branch: %s took %.3f ms", step,
		 INSTR_TIME_GET_MILLISEC(now));
	INSTR_TIME_SET_CURRENT(*start);
}

/*
 * pg_branch(target text, checkpoint bool) returns pg_lsn
 *
 * Copy the cluster into a new data directory, without interrupting it.
 * Returns the WAL position the copy is consistent as of.  The copy shares
 * the cluster's page store layers by hard link, so the target must be on
 * the same file system as the data directory, and must not exist yet.
 * Unless checkpoint is false, a checkpoint first keeps the WAL the branch
 * replays when it starts short.
 */
Datum
pg_branch(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		checkpoint = PG_GETARG_BOOL(1);
	instr_time	start;
	ControlFileData cf;
	ControlFileData cf_again;
	uint32		layers[PAGESTORE_MAX_LAYERS];
	int			nlayers;
	XLogRecPtr	branchpoint;
	DIR		   *dir;
	struct dirent *de;

	if (!PageStoreEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_branch() requires a cluster that uses the page store"),
				 errhint("Create the cluster with \"initdb --pagestore\".")));
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("pg_branch() cannot be executed during recovery.")));
	if (!fullPageWrites)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_branch() requires \"full_page_writes\" to be enabled")));
	if (!XLogHintBitIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_branch() requires data checksums or \"wal_log_hints\"")));
	if (!is_absolute_path(target))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("target directory must be an absolute path")));
	canonicalize_path(target);
	if (path_is_prefix_of_path(DataDir, target))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("target directory must not be inside the data directory")));

	/* Relations in other tablespaces are not in the page store */
	dir = AllocateDir(PG_TBLSPC_DIR);
	while ((de = ReadDir(dir, PG_TBLSPC_DIR)) != NULL)
	{
		if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pg_branch() does not support tablespaces")));
	}
	FreeDir(dir);

	INSTR_TIME_SET_CURRENT(start);
	if (checkpoint)
	{
		/*
		 * Forced: an unforced checkpoint is skipped if nothing was logged
		 * since the last one finished, but a spread checkpoint's redo point
		 * can be far behind its end.
		 */
		RequestCheckpoint(CHECKPOINT_FAST | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
		branch_step_done("checkpoint", &start);
	}

	if (MakePGDirectory(target) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", target)));

	/* one branch at a time: there is one place to keep WAL for it */
	LWLockAcquire(PageStoreBranchLock(), LW_EXCLUSIVE);

	PG_ENSURE_ERROR_CLEANUP(branch_failed, CStringGetDatum(target));
	{
		/*
		 * 1. Keep the WAL from the last checkpoint's redo point.  A
		 * checkpoint that completes after we read the control file could
		 * remove that WAL before seeing what we keep, so read it again: if it
		 * changed, start from the newer checkpoint.  If not, checkpoints
		 * completing from now on see what we keep.
		 */
		GetControlFileCopy(&cf);
		for (;;)
		{
			PageStoreSetBranchKeepLSN(cf.checkPointCopy.redo);
			GetControlFileCopy(&cf_again);
			if (cf_again.checkPoint == cf.checkPoint)
				break;
			cf = cf_again;
		}
		if (!cf.checkPointCopy.fullPageWrites)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the last checkpoint was made with \"full_page_writes\" disabled"),
					 errhint("Run CHECKPOINT and try again.")));
		if (cf.checkPointCopy.ThisTimeLineID != GetWALInsertionTimeLine())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the last checkpoint is on a previous timeline"),
					 errhint("Run CHECKPOINT and try again.")));

		/* 2. Copy what's outside the page store and pg_wal */
		branch_copy_dir(".", target, true);
		branch_step_done("copying files", &start);

		/* 3. Freeze the page store, and 4. take the branch point */
		nlayers = PageStoreFreeze(layers, &branchpoint);
		branch_step_done("freezing", &start);
		XLogFlush(branchpoint);

		branch_link_layers(target, layers, nlayers);
		branch_step_done("linking layers", &start);
		branch_copy_wal(target, cf.checkPointCopy.ThisTimeLineID,
						cf.checkPointCopy.redo, branchpoint);
		branch_step_done("copying WAL", &start);

		/*
		 * The branch's WAL diverges from ours under the same file names, so
		 * it mustn't go to our archive.
		 */
		branch_disable_archiving(target);

		/* Last, the control file that makes it a crashed server's */
		cf.state = DB_IN_PRODUCTION;
		cf.minRecoveryPoint = InvalidXLogRecPtr;
		cf.minRecoveryPointTLI = 0;
		cf.backupStartPoint = InvalidXLogRecPtr;
		cf.backupEndPoint = InvalidXLogRecPtr;
		cf.backupEndRequired = false;
		branch_write_control_file(target, &cf);
		fsync_fname(target, true);
	}
	PG_END_ENSURE_ERROR_CLEANUP(branch_failed, CStringGetDatum(target));

	PageStoreSetBranchKeepLSN(InvalidXLogRecPtr);
	LWLockRelease(PageStoreBranchLock());

	PG_RETURN_LSN(branchpoint);
}

/*
 * pg_pagestore_reclaim() returns record
 *
 * Give back the space of frozen page store layers that the cluster can no
 * longer read; see PageStoreReclaim().  The checkpointer does the same after
 * checkpoints, unless pagestore_reclaim is off.
 */
Datum
pg_pagestore_reclaim(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false};
	PageStoreReclaimResult result;

	if (!PageStoreEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the cluster does not use the page store")));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	(void) PageStoreReclaim(&result, true);

	values[0] = Int64GetDatum(result.removed_files);
	values[1] = Int32GetDatum(result.removed_layers);
	values[2] = Int64GetDatum(result.reclaimed_bytes);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc),
													  values, nulls)));
}
