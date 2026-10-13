# The page store

The page store is a copy-on-write storage manager for PostgreSQL. With it, a
running cluster, or one database in it, can be copied in about the same time
whatever its size: copies share the original's files until either side
writes to them. Nothing about the file system is required beyond hard links,
so it works the same on APFS, ext4, xfs, NTFS, and inside rootless containers.

It was built for Encore's local development: branching a developer's whole
database while the app keeps running, and giving every test its own copy of a
migrated database in milliseconds.

This is a prototype, on top of `REL_19_STABLE`. See [Limitations](#limitations)
before relying on it.

- [Getting started](#getting-started)
- [Features](#features)
  - [Branching a cluster](#branching-a-cluster)
  - [Cloning a database](#cloning-a-database)
  - [Snapshots and restores of a database](#snapshots-and-restores-of-a-database)
  - [Freezing](#freezing)
  - [Giving space back](#giving-space-back)
  - [Merging layers](#merging-layers)
- [Settings](#settings)
- [Functions](#functions)
- [How it works](#how-it-works)
- [Performance](#performance)
- [Limitations](#limitations)
- [Building and testing](#building-and-testing)

## Getting started

Create a cluster with `initdb --pagestore`. Branching also needs data
checksums (or `wal_log_hints = on`) and `full_page_writes = on`, the default:

```sh
initdb --pagestore --data-checksums -D data
pg_ctl -D data -l logfile start
```

Relations in `pg_default` and `pg_global` are then kept in the page store, in
`data/pg_pagestore` rather than `data/base`. Temporary relations, and
relations in other tablespaces, stay with the usual storage manager. A
cluster made without `--pagestore` works exactly as stock PostgreSQL does, and
an existing cluster can't be converted: dump and restore it into a new one.

## Features

### Branching a cluster

`pg_branch(target)` copies the whole cluster, as of now, into a new data
directory, while it keeps running and taking writes:

```sql
SELECT pg_branch('/path/to/branch');   -- returns the WAL position it's as of
```

```sh
echo "port = 5433" >> /path/to/branch/postgresql.conf
pg_ctl -D /path/to/branch -l branch.log start
```

The branch starts like a cluster that crashed at that moment: it replays the
WAL `pg_branch` copied into it, rolls back the transactions that were running,
and is then a consistent, independent cluster. It has the source's settings,
so give it a port of its own before starting it. Archiving is turned off in
its `postgresql.auto.conf`, so that it doesn't write into the source's
archive.

- **Cost:** a checkpoint, then linking the page store's files and copying the
  small files outside it (configuration, the control file, the WAL to
  replay). Idle, it takes 12 to 30 ms whatever the cluster's size. Right
  after heavy writes the checkpoint dominates: 0.4 to 2.3 s on a busy
  1.5 GB cluster. `pg_branch(target, false)` skips the checkpoint; the
  branch then replays more WAL when it starts.
- **Space:** none until the source or the branch writes. On macOS, layer
  directories are cloned with `clonefile`; elsewhere their files are
  hard-linked.
- **The target** must be an absolute path on the same file system as the
  data directory, outside it, and must not exist yet.
- **Requirements:** a page store cluster, not in recovery, with
  `full_page_writes` and data checksums or `wal_log_hints`, and no
  tablespaces other than `pg_default` and `pg_global`.

### Cloning a database

`CREATE DATABASE ... STRATEGY = SNAPSHOT` creates a database that shares its
template's files, so that it takes about the same time whatever the
template's size, needs no checkpoint, and writes a single small WAL record:

```sql
CREATE DATABASE test_1 TEMPLATE app_template STRATEGY = SNAPSHOT;
```

As with every `CREATE DATABASE`, nobody else may be connected to the template.
Writes to the clone or to the template afterwards go to files of their own;
neither sees the other's.

Clone times measured on a release build (macOS, APFS), including about 11 ms
of round trip, median of 9:

| Template | `SNAPSHOT` | `FILE_COPY` | `WAL_LOG` |
|---|---|---|---|
| 8 MB | 45 ms | 45 ms | 63 ms |
| 113 MB | 76 ms | 149 ms | 399 ms |
| 1.1 GB | 83 ms | 1,155 ms | 1,247 ms |

**Freeze templates.** A clone shares what its template had when the page
store was last frozen (see [Freezing](#freezing)). What the template wrote
since is copied into the clone, up to 16 MB; beyond that, or when the page
store's tables have no room for it, the clone freezes first. Freezing a
template once it's ready makes every clone of it a matter of links only, and
clones then take no room in the page store's tables until they're written to:

```sql
ALTER DATABASE app_template ALLOW_CONNECTIONS false;
SELECT pg_pagestore_freeze('app_template');
```

Reading a database counts as writing to it here, because reading sets hint
bits on rows written since the last vacuum. Freeze after the last read, or
clone from a template nobody reads.

The template and the new database must keep all their relations in
`pg_default`. On a cluster without the page store, the strategy is refused.

### Snapshots and restores of a database

With `STRATEGY = SNAPSHOT`, a snapshot of a database is a clone of it, and a
restore is a clone of the snapshot under the original's name. Both take
about 100 ms, whatever the size. `CREATE DATABASE` can't run in a function or
a transaction block, so a client runs the steps:

```sql
-- snapshot: disconnects the database's clients for a moment
ALTER DATABASE app ALLOW_CONNECTIONS false;
SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = 'app';
-- wait until pg_stat_activity has no sessions for 'app'
CREATE DATABASE app_snap TEMPLATE app STRATEGY = SNAPSHOT;
ALTER DATABASE app ALLOW_CONNECTIONS true;

-- restore
ALTER DATABASE app ALLOW_CONNECTIONS false;
SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = 'app';
-- wait until pg_stat_activity has no sessions for 'app'
ALTER DATABASE app RENAME TO app_old;
CREATE DATABASE app TEMPLATE app_snap STRATEGY = SNAPSHOT;
DROP DATABASE app_old;   -- later: DROP DATABASE forces a checkpoint
```

Restore from a snapshot nobody has read or written, so that the clone needs
nothing copied or frozen. To look into a snapshot, clone it and look at the
clone.

### Freezing

The page store keeps a cluster's relations in a stack of layers. Writes go to
the top layer; the layers below are frozen and never change, which is what
lets branches and clones share them. Freezing makes the top layer frozen and
starts a new, empty one.

```sql
SELECT pg_pagestore_freeze();          -- returns the number of frozen layers
SELECT pg_pagestore_freeze('app');     -- writes out app's buffers first
```

Freezing fsyncs what the top layer had written since its last fsync, so it
costs little right after a checkpoint. Several things freeze besides
`pg_pagestore_freeze`:

- `pg_branch`, every time, as it shares the frozen layers.
- `initdb --pagestore`, at its end, so that a new cluster starts with an
  empty top layer and its template databases frozen.
- `CREATE DATABASE ... STRATEGY = SNAPSHOT`, when its template wrote more
  since the last freeze than it copies.
- The page store merger, when `pagestore_auto_freeze` is on and the top
  layer's tables are 75% full (see below).

The top layer keeps what it knows about each relation fork written since the
last freeze in shared memory: one entry per fork, `pagestore_max_forks` of
them, and a 1 kB bitmap chunk per 8,192 blocks of a fork written over its
frozen version, `pagestore_max_bitmap_chunks` of them; forks of at most 256
blocks keep their bits in their entry. Freezing empties both. With
`pagestore_auto_freeze` on (the default), they don't run out: the merger
freezes at 75%, and a write that still finds one full freezes and retries.
With it off, a write or `CREATE TABLE` that finds no room fails with "out of
page store fork entries" or "out of page store bitmap chunks", and the rest
of the cluster carries on; the checkpointer and the startup process still
freeze rather than fail.

### Giving space back

A block written over in a later frozen layer can no longer be read from the
layer below, unless a branch or clone still reads that layer. After each
checkpoint (`pagestore_reclaim = on`, the default), the checkpointer gives
that space back:

- it removes files that hold nothing readable any more, such as those of
  dropped relations and databases;
- it punches holes in files where blocks are hidden, if no other name links
  to the file;
- it takes layers left empty off the stack.

`SELECT * FROM pg_pagestore_reclaim()` does the same on demand, and reports
the files and layers removed and the bytes given back. Until the next freeze,
a block written since the last one exists twice: in the top layer and in the
frozen layer below. With many layers and steady writes, a cluster can take up
to about twice the space of the same data in stock PostgreSQL.

### Merging layers

Every freeze adds a frozen layer, and reading a block may look through all of
them. The page store merger, a background worker, merges adjacent frozen
layers when there are more than `pagestore_max_frozen_layers` (8 by
default; 0 turns it off). `SELECT * FROM pg_pagestore_merge(n)` merges down
to `n` layers on demand.

A merge writes a new layer that reads the same as the pair it replaces.
Files only one of the pair has are linked into it. Files both have are
written anew, starting from a copy-on-write clone where the file system has
one, and a file several databases share is written once and linked for each.
Branches keep the layers they share. Merging and reclaiming in the background
stop early when a `CREATE DATABASE` waits for them, and pick up later.

## Settings

| Setting | Default | Set at | What it does |
|---|---|---|---|
| `pagestore_max_forks` | 65536 | server start | Fork entries in the top layer's table, about 88 bytes of shared memory each (5.6 MB at the default) |
| `pagestore_max_bitmap_chunks` | 16384 | server start | 1 kB bitmap chunks for forks over 256 blocks written over their frozen version (17 MB at the default) |
| `pagestore_auto_freeze` | on | reload | Freeze the top layer when either table is 75% full |
| `pagestore_max_frozen_layers` | 8 | reload | Merge frozen layers in the background beyond this many; 0 turns merging off |
| `pagestore_reclaim` | on | reload | Give space back after checkpoints |

## Functions

All are restricted to superusers by default; others can be granted EXECUTE.

| Function | Returns | What it does |
|---|---|---|
| `pg_branch(target text, checkpoint bool DEFAULT true)` | `pg_lsn` | Copies the cluster into a new data directory; returns the WAL position it's consistent as of |
| `pg_pagestore_freeze(database name DEFAULT NULL)` | `integer` | Freezes the top layer, after writing out the database's buffers if one is given; returns the number of frozen layers |
| `pg_pagestore_reclaim()` | `(removed_files bigint, removed_layers integer, reclaimed_bytes bigint)` | Gives back the space of what the cluster can no longer read |
| `pg_pagestore_merge(max_layers integer DEFAULT NULL)` | `(merged_layers integer, written_bytes bigint)` | Merges frozen layers until there are at most `max_layers`, or `pagestore_max_frozen_layers` |

And the strategy `CREATE DATABASE ... STRATEGY = SNAPSHOT`, described above.

## How it works

The page store is a storage manager, next to `md.c`, behind PostgreSQL's
`smgr` interface: the buffer manager, WAL and everything above work as
before. The code is in `src/backend/storage/smgr/pagestore.c` and
`pagestore_branch.c`.

**Layers.** `pg_pagestore/STACK` lists the layers, bottom first; each is a
directory `pg_pagestore/<generation>/`. A relation fork's data in a layer is
the file `<tablespace>_<database>_<relfilenode>_<fork>`, block N at offset
N × 8 kB, sparse. An optional `.map` file next to it says which blocks the
layer holds: all from its *base* on, and those below the base set in a
bitmap; or that the fork was dropped, which hides the layers below. Reading
a block takes it from the topmost layer that holds it.

**Writing** always goes to the top layer. The first write of a fork there
makes its files, with a base equal to the fork's size in the layers below;
blocks below the base that get written are recorded in the bitmap, in shared
memory until a checkpoint writes the map out. Truncating lowers the base.

**Freezing** writes out the top layer's maps, fsyncs what it wrote, and adds
a new, empty top layer to the stack. Frozen layers never change again, except
by reclaiming, which only removes what nobody can read, and merging, which
replaces a pair by a layer that reads the same.

**Branching** checkpoints, freezes, links every frozen layer's files into the
target, copies the files outside the page store, and copies the WAL from the
checkpoint up to the freeze, so that the branch's crash recovery makes up for
whatever the frozen files caught half-written.

**Cloning a database** links each of the template's files in the frozen
layers under the new database's name. The links are durable before the WAL
record of the creation is written, so replaying it after a crash, or in a
branch, finds them there and does nothing; a standby, which has no such
files, copies the template instead.

**Sharing and space.** Files are shared by hard link. Reclaiming punches holes
only in files no other name links to, so a block one database or branch no
longer reads stays for another that does. On macOS, branches clone layer
directories with `clonefile` instead, which shares storage between separate
files.

**Crash safety** follows PostgreSQL's rules: data reaches disk after its WAL,
and full page writes repair torn pages. On top of that, a map is durable
before the data file whose base it gives, and the stack file is replaced
atomically. Layer directories not in the stack, left by a crash, are removed
at startup.

## Performance

`pgbench` at scale 100 (1.5 GB), 8 clients, release builds, macOS on an M5
Pro, median of 5 interleaved 30 s runs against stock `REL_19_STABLE`:

| Cluster | Read-only | Read-write |
|---|---|---|
| This build, without `--pagestore` | −0.3% | +0.1% |
| Page store, no frozen layers | −1.5% | −1.4% |
| Page store, 8 frozen layers | −1.4% | −1.1% |
| Page store, merged to 1 | −1.8% | inconclusive (noisy run) |

Merging 9 layers of that cluster into 2 took 30 s and wrote 1.7 GB.

## Limitations

- **Prototype.** It has run on macOS (APFS), and branching also on Linux
  (xfs, in Podman). Database cloning and the table limits have only run on
  macOS; Windows and ext4 have not been tried at all.
- **Tablespaces.** Only `pg_default` and `pg_global` are in the page store.
  `pg_branch` refuses clusters with other tablespaces, `STRATEGY = SNAPSHOT`
  refuses databases with relations in them, and `ALTER DATABASE ... SET
  TABLESPACE` is refused in a page store cluster.
- **Tools.** `pg_upgrade`, `pg_rewind` and `pg_checksums` work on relations'
  files in `base/`, don't know the page store's layout, and aren't
  supported. `pg_basebackup` and standbys haven't been tried.
- **Branches** keep the source's system identifier and timeline. `pg_branch`
  can't be cancelled while it holds its lock.
- **Clones** need their template free of connections, as any `CREATE
  DATABASE` does; snapshotting a database in use means disconnecting its
  clients briefly. On macOS, linking costs about 0.2 ms per file, about 60 ms
  for a fresh database.
- **Space.** Blocks rewritten since the last freeze are stored twice until the
  next freeze and reclaim. On Linux, merging layers that branches still link
  to duplicates them until those branches go.
- **Restarts.** Lowering `pagestore_max_forks` or
  `pagestore_max_bitmap_chunks` below what the top layer holds on disk stops
  the server from starting until it's raised again.
- **Catalog version.** The page store's functions changed the catalog, so a
  build of this branch doesn't start on data directories of stock 19, nor on
  ones made by earlier builds of it.

## Building and testing

Build as PostgreSQL usually builds. The TAP tests need `--enable-tap-tests`:

```sh
./configure --enable-tap-tests --enable-depend --prefix=$PWD/install
make -j && make install
```

Tests of the page store:

```sh
# the recovery TAP tests, among them the page store's
make -C src/test/recovery check \
  PROVE_TESTS="t/060_pagestore_branch.pl t/061_pagestore_snapshot_database.pl t/062_pagestore_limits.pl"

# the regression tests on a page store cluster; only "database" fails, as
# ALTER DATABASE ... SET TABLESPACE is refused
PG_TEST_INITDB_EXTRA_OPTS=--pagestore make check
```

- `060_pagestore_branch.pl`: branching under load, crashes, reclaiming,
  merging.
- `061_pagestore_snapshot_database.pl`: cloning databases, crash recovery and
  replay of a clone, merging and dropping clones.
- `062_pagestore_limits.pl`: full tables, freezing, automatic freezing.

### The build Encore bundles

`encore/dist.sh <output dir>` uses meson to build the server, the client
programs and contrib into a directory that runs from wherever it's moved to.
It then checks a copy of that directory by initializing a page store cluster
and cloning a database. It builds for darwin-arm64, darwin-amd64 (on an Apple
Silicon Mac), linux-amd64, linux-arm64 and windows-amd64; the script lists
what each needs. The Linux builds are made in a `manylinux_2_28` container,
so they run on glibc 2.28 and later. Encore's releaser runs the script for
each platform and publishes the results, as `cmd/postgres` in
`encoredev/releaser` describes.
