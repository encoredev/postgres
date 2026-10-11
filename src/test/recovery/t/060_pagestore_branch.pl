
# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for pg_branch(): copying a cluster that uses the page store while it
# is busy, and that the copy is consistent and independent of the source.
use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $source = PostgreSQL::Test::Cluster->new('source');
$source->init(extra => ['--pagestore']);
$source->append_conf('postgresql.conf', 'checkpoint_timeout = 30s');
$source->start;

# Branch $from into a new, started node.
sub branch
{
	my ($from, $name) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	my $lsn = $from->safe_psql('postgres',
		"SELECT pg_branch('" . $node->data_dir . "')");
	$node->append_conf('postgresql.conf', 'port = ' . $node->port);
	$node->start;
	return ($node, $lsn);
}

# The pgbench tables' balances add up the same way however many
# transactions ran, if every transaction is in whole or not at all.
sub check_balances
{
	my ($node, $name) = @_;
	my $sums = $node->safe_psql(
		'postgres', q{
		SELECT (SELECT sum(abalance) FROM pgbench_accounts) || ',' ||
		       (SELECT sum(bbalance) FROM pgbench_branches) || ',' ||
		       (SELECT sum(tbalance) FROM pgbench_tellers) || ',' ||
		       (SELECT coalesce(sum(delta), 0) FROM pgbench_history)});
	my @sums = split /,/, $sums;
	ok($sums[0] == $sums[1] && $sums[1] == $sums[2] && $sums[2] == $sums[3],
		"$name: balances agree ($sums)");
}

# Check every index against its table, and that index and sequential scans
# see the same rows.
sub check_relations
{
	my ($node, $name) = @_;
	$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS amcheck');
	# bt_index_check() raises an error on corruption
	my $checked = $node->safe_psql(
		'postgres', q{
		SELECT count(bt_index_check(i.oid, true)::text) FROM
		  (SELECT c.oid FROM pg_class c JOIN pg_am am ON am.oid = c.relam
		   WHERE c.relkind = 'i' AND am.amname = 'btree'
		     AND c.relpersistence = 'p' OFFSET 0) i});
	ok($checked > 0, "$name: $checked btree indexes match their tables");
	is( $node->safe_psql(
			'postgres', q{
		SET enable_seqscan = off;
		SELECT count(*) FROM pgbench_accounts WHERE aid > 0}),
		$node->safe_psql(
			'postgres', q{
		SET enable_indexscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM pgbench_accounts}),
		"$name: index and sequential scans agree");
}

$source->command_ok(
	[ 'pgbench', '--initialize', '--scale' => '5', $source->connstr('postgres') ],
	'pgbench initialize');

# A table to drop, an unlogged table, and a table to rewrite after branching
$source->safe_psql(
	'postgres', q{
	CREATE TABLE dropme AS SELECT g AS a FROM generate_series(1, 1000) g;
	CREATE UNLOGGED TABLE unlogged AS SELECT g AS a FROM generate_series(1, 1000) g;
	CREATE TABLE rewriteme AS SELECT g AS a FROM generate_series(1, 1000) g;
	});

# Branch while pgbench runs, around checkpoints and without them.
my ($stdout, $stderr) = ('', '');
my $pgbench = IPC::Run::start(
	[
		'pgbench', '--no-vacuum', '--client' => '4',
		'--jobs' => '2', '--time' => '8',
		$source->connstr('postgres')
	],
	'>' => \$stdout,
	'2>' => \$stderr);

usleep(1_500_000);
my ($busy1, $lsn1) = branch($source, 'busy1');
$source->safe_psql('postgres', 'CHECKPOINT');
my ($busy2, $lsn2) = branch($source, 'busy2');
usleep(1_500_000);
my ($busy3, $lsn3) = branch($source, 'busy3');

$pgbench->finish;
is($pgbench->result(0), 0, 'pgbench ran while branching');

foreach my $node ($busy1, $busy2, $busy3)
{
	check_balances($node, $node->name);
	check_relations($node, $node->name);
}
check_balances($source, 'source');

# A branch has every transaction that committed before its branch point,
# and none after: the history table only grows.
my $hist1 = $busy1->safe_psql('postgres', 'SELECT count(*) FROM pgbench_history');
my $hist3 = $busy3->safe_psql('postgres', 'SELECT count(*) FROM pgbench_history');
my $hist_source =
  $source->safe_psql('postgres', 'SELECT count(*) FROM pgbench_history');
ok($hist1 > 0 && $hist1 <= $hist3 && $hist3 <= $hist_source,
	"later branches have more history ($hist1, $hist3, $hist_source)");

# A branch's WAL diverges, so it must not archive into the source's archive
is($busy1->safe_psql('postgres', 'SHOW archive_mode'),
	'off', 'archiving is off in a branch');

# The unlogged table is empty in a branch, as after a crash
is($busy1->safe_psql('postgres', 'SELECT count(*) FROM unlogged'),
	'0', 'unlogged table is reset in a branch');
is($source->safe_psql('postgres', 'SELECT count(*) FROM unlogged'),
	'1000', 'unlogged table is untouched in the source');

# Changes after branching stay on their side
$source->safe_psql(
	'postgres', q{
	DROP TABLE dropme;
	VACUUM FULL rewriteme;
	DELETE FROM rewriteme WHERE a > 10;
	TRUNCATE pgbench_history;
	});
$busy1->safe_psql(
	'postgres', q{
	INSERT INTO rewriteme SELECT g FROM generate_series(1001, 2000) g;
	UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid <= 100;
	});
is($busy1->safe_psql('postgres', 'SELECT count(*) FROM dropme'),
	'1000', 'table dropped in the source survives in the branch');
is($busy1->safe_psql('postgres', 'SELECT count(*) FROM rewriteme'),
	'2000', 'branch sees its own writes only');
is($source->safe_psql('postgres', 'SELECT count(*) FROM rewriteme'),
	'10', 'source sees its own writes only');
is($busy2->safe_psql('postgres', 'SELECT count(*) FROM rewriteme'),
	'1000', 'another branch sees neither');
ok( $busy3->safe_psql('postgres', 'SELECT count(*) FROM pgbench_history') ==
	  $hist3,
	'truncating in the source leaves a branch alone');

# A branch of a branch
my ($nested, $nested_lsn) = branch($busy1, 'nested');
is($nested->safe_psql('postgres', 'SELECT count(*) FROM rewriteme'),
	'2000', 'branch of a branch has the branch\'s writes');
check_relations($nested, 'nested');

# Branching twice with nothing written in between adds no layer
$busy2->safe_psql('postgres', 'CHECKPOINT');
my ($quiet1) = branch($busy2, 'quiet1');
my $stack1 = slurp_file($quiet1->data_dir . '/pg_pagestore/STACK');
my ($quiet2) = branch($busy2, 'quiet2');
my $stack2 = slurp_file($quiet2->data_dir . '/pg_pagestore/STACK');
is($stack2, $stack1, 'no new layer when nothing was written');

# The branch has the source's frozen layers, by hard link or clone.  Either
# may have reclaimed hidden files of them since, so compare what both have.
my @layers = split /\s+/, $stack1;
my $shared_layer = $layers[0];
my $source_layer = $busy2->data_dir . "/pg_pagestore/$shared_layer";
my $branch_layer = $quiet1->data_dir . "/pg_pagestore/$shared_layer";
opendir(my $dh, $source_layer) or die "could not open layer: $!";
my @common = grep { !/^\./ && -e "$branch_layer/$_" } readdir($dh);
closedir($dh);
ok(@common > 100, 'branch shares the frozen layer (' . scalar(@common) . ' files)');
my @differ = grep {
	slurp_file("$branch_layer/$_") ne slurp_file("$source_layer/$_")
} @common;
is_deeply(\@differ, [], 'with the same contents');
if ($^O ne 'darwin')
{
	my @copies = grep {
		(stat("$branch_layer/$_"))[1] != (stat("$source_layer/$_"))[1]
	} @common;
	is_deeply(\@copies, [], 'by hard link');
}

# The source survives a crash after all this, and so do its branches' data
$source->safe_psql('postgres',
	'INSERT INTO rewriteme SELECT g FROM generate_series(11, 20) g');
$source->kill9;
$source->start;
is($source->safe_psql('postgres', 'SELECT count(*) FROM rewriteme'),
	'20', 'source recovers from a crash');
check_relations($source, 'source after crash');
check_relations($busy1, 'busy1 after the source crashed');

# A failed branch leaves nothing behind
my $exists = $source->data_dir;
my ($ret, $out, $err) = $source->psql('postgres',
	"SELECT pg_branch('$exists')");
isnt($ret, 0, 'branching into the data directory fails');
ok(-f "$exists/PG_VERSION", 'and leaves the existing directory alone');

# Reclaiming the space of frozen layers the cluster can no longer read
my $rc = PostgreSQL::Test::Cluster->new('reclaim');
$rc->init(extra => ['--pagestore']);
$rc->append_conf('postgresql.conf', 'pagestore_reclaim = off');
$rc->start;
$rc->safe_psql(
	'postgres', q{
	CREATE EXTENSION amcheck;
	CREATE TABLE r (id int PRIMARY KEY, pad text);
	INSERT INTO r SELECT g, repeat('a', 200) FROM generate_series(1, 50000) g;
	CREATE TABLE gone AS SELECT g FROM generate_series(1, 50000) g;
	});

sub reclaim
{
	my ($node) = @_;
	return split /\|/,
	  $node->safe_psql('postgres',
		'SELECT removed_files, removed_layers, reclaimed_bytes FROM pg_pagestore_reclaim()'
	  );
}

sub check_r
{
	my ($node, $pad, $name) = @_;
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*), count(*) FILTER (WHERE pad = repeat('$pad', 200)) FROM r"
		),
		'50000|50000',
		"$name: table intact");
	$node->safe_psql('postgres', "SELECT bt_index_check('r_pkey', true)");
}

# A branch that stays, sharing the layer the rewrite below hides
my ($keep) = branch($rc, 'reclaim_keep');
$rc->safe_psql('postgres',
	q{UPDATE r SET pad = repeat('b', 200); DROP TABLE gone;});

# A branch that goes, freezing the rewrite and the drop
my ($gone) = branch($rc, 'reclaim_gone');
$gone->stop;
rmtree($gone->data_dir);

my ($files, $layers, $bytes) = reclaim($rc);
ok($files > 0, "reclaiming removes files hidden by later layers ($files)");
check_r($rc, 'b', 'source after reclaim');
check_r($keep, 'a', 'branch sharing reclaimed layers');
is($keep->safe_psql('postgres', 'SELECT count(*) FROM gone'),
	'50000', 'branch keeps a table the source dropped and reclaimed');

my ($files2, $layers2, $bytes2) = reclaim($rc);
is($files2, '0', 'reclaiming again finds nothing more');

# With the branch gone, nothing shares the hidden blocks any more
$keep->stop;
rmtree($keep->data_dir);
my ($files3, $layers3, $bytes3) = reclaim($rc);
ok($bytes + $bytes3 > 0,
	'reclaiming frees space (' . ($bytes + $bytes3) . ' bytes)');

$rc->restart;
check_r($rc, 'b', 'source after reclaim and restart');
my ($after) = branch($rc, 'reclaim_after');
check_r($after, 'b', 'branch taken after reclaiming');

# The checkpointer reclaims by itself
$rc->append_conf('postgresql.conf', 'pagestore_reclaim = on');
$rc->reload;
$rc->safe_psql('postgres', q{UPDATE r SET pad = repeat('c', 200)});
my ($last) = branch($rc, 'reclaim_last');
my $log_offset = -s $rc->logfile;
$rc->safe_psql('postgres', 'CHECKPOINT');

# It does so after letting the CHECKPOINT command return
$rc->wait_for_log(qr/page store reclaimed/, $log_offset);
pass('checkpointer reclaims after a checkpoint');
check_r($rc, 'c', 'source after the checkpointer reclaimed');
check_r($last, 'c', 'branch after the checkpointer reclaimed');

# A layer holding nothing but writes to a table dropped later is emptied and
# taken off the stack; checkpoints must cope with sync requests for it.
my $el = PostgreSQL::Test::Cluster->new('empty_layer');
$el->init(extra => ['--pagestore']);
$el->append_conf('postgresql.conf',
	"autovacuum = off\npagestore_reclaim = off\ncheckpoint_timeout = 1h");
$el->start;
$el->safe_psql('postgres',
	'CREATE TABLE e AS SELECT g FROM generate_series(1, 10000) g; CHECKPOINT');
my $el_dir = PostgreSQL::Test::Utils::tempdir;
$el->safe_psql('postgres', "SELECT pg_branch('$el_dir/1', false)");
$el->safe_psql('postgres', 'INSERT INTO e SELECT g FROM generate_series(1, 10000) g');
$el->safe_psql('postgres', "SELECT pg_branch('$el_dir/2', false)");
$el->safe_psql('postgres', 'DROP TABLE e');
$el->safe_psql('postgres', "SELECT pg_branch('$el_dir/3', false)");
is($el->safe_psql('postgres', "SELECT to_regclass('e') IS NULL"),
	't', 'table is dropped before reclaiming');
my @stack_before = split ' ', slurp_file($el->data_dir . '/pg_pagestore/STACK');
my (undef, $el_layers) = reclaim($el);
my @stack_after = split ' ', slurp_file($el->data_dir . '/pg_pagestore/STACK');
my ($layers_before, $layers_after) = (scalar(@stack_before), scalar(@stack_after));
is($el->safe_psql('postgres', "SELECT to_regclass('e') IS NULL"),
	't', 'table is still dropped after reclaiming');
ok($el_layers >= 1 && $layers_after < $layers_before,
	"empty layers are taken off the stack ($layers_before -> $layers_after)");
$el->safe_psql('postgres', 'CHECKPOINT');
$el->restart;
is($el->safe_psql('postgres', "SELECT to_regclass('e') IS NULL"),
	't', 'checkpoint and restart after removing a layer');

done_testing();
