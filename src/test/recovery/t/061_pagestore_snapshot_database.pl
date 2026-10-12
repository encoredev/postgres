# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for CREATE DATABASE ... STRATEGY SNAPSHOT: a database that shares its
# template's page store files, and stays independent of it.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(extra => ['--pagestore']);
# No checkpoint or merge but those the test makes
$node->append_conf(
	'postgresql.conf', q{
checkpoint_timeout = 1h
max_wal_size = 4GB
pagestore_max_frozen_layers = 0
});
$node->start;

sub layers
{
	my $stack = slurp_file($node->data_dir . '/pg_pagestore/STACK');
	return split ' ', $stack;
}

# The inode of a file in a layer, failing if there's none
sub inode
{
	my ($gen, $db, $rfn) = @_;
	my $path = $node->data_dir . "/pg_pagestore/$gen/1663_${db}_${rfn}_main";
	my @st = stat($path) or die "could not stat $path: $!";
	return $st[1];
}

sub dboid
{
	my ($db) = @_;
	return $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = '$db'");
}

# What the template holds, in a form to compare; its unlogged table apart,
# which a crash empties
sub contents
{
	my ($db) = @_;
	return $node->safe_psql(
		$db, q{
		SELECT (SELECT count(*) || ':' || md5(string_agg(a || '/' || b, ',' ORDER BY a)) FROM t)
		  || ' ' || (SELECT last_value FROM s)
		  || ' ' || (SELECT f())
		  || ' ' || (SELECT count(*) FROM v)});
}

sub create
{
	my ($db, $template) = @_;
	$node->safe_psql('postgres',
		"CREATE DATABASE $db TEMPLATE $template STRATEGY = SNAPSHOT");
}

sub create_with_oid
{
	my ($db, $template, $oid) = @_;
	$node->safe_psql('postgres',
		"CREATE DATABASE $db OID = $oid TEMPLATE $template STRATEGY = SNAPSHOT");
}

# bt_index_check() raises an error on corruption
sub check_indexes
{
	my ($db, $name) = @_;
	$node->safe_psql($db, 'CREATE EXTENSION IF NOT EXISTS amcheck');
	my $checked = $node->safe_psql(
		$db, q{
		SELECT count(bt_index_check(c.oid, true)::text)
		FROM pg_class c JOIN pg_am am ON am.oid = c.relam
		WHERE c.relkind = 'i' AND am.amname = 'btree' AND c.relpersistence = 'p'});
	ok($checked > 0, "$name: $checked btree indexes match their tables");
}

$node->safe_psql('postgres', 'CREATE DATABASE tmpl');
$node->safe_psql(
	'tmpl', q{
	CREATE TABLE t (a int PRIMARY KEY, b text);
	INSERT INTO t SELECT g, repeat(md5(g::text), 4) FROM generate_series(1, 20000) g;
	CREATE INDEX t_b ON t (b);
	CREATE UNLOGGED TABLE u AS SELECT g FROM generate_series(1, 500) g;
	CREATE SEQUENCE s;
	SELECT setval('s', 42);
	CREATE FUNCTION f() RETURNS int LANGUAGE plpgsql AS 'BEGIN RETURN 7; END';
	CREATE VIEW v AS SELECT * FROM t WHERE a <= 10;
	CREATE TABLE dropped AS SELECT 1 AS a;
	DROP TABLE dropped;
});
my $tmpl = contents('tmpl');
my $orig = $tmpl;

# The template's writes are all in the top layer, and few: the first copy
# copies them rather than freeze the top layer
my @before = layers();
create('c1', 'tmpl');
is(scalar(layers()), scalar(@before), 'copy of a small template adds no layer');
is(contents('c1'), $tmpl, 'copy has the template\'s contents');
is($node->safe_psql('c1', 'SELECT count(*) FROM u'),
	'500', 'copy has the template\'s unlogged table');
check_indexes('c1', 'copy');

# Once frozen, a copy shares the template's files.  A table the template
# dropped since stays dropped in the copy too.
$node->safe_psql('tmpl', 'CREATE TABLE gone AS SELECT 1 AS a');
my $gone_rfn = $node->safe_psql('tmpl', "SELECT pg_relation_filenode('gone')");
$node->safe_psql('postgres', "SELECT pg_branch('" . $node->basedir . "/freeze0', false)");
$node->safe_psql('tmpl', 'DROP TABLE gone');
my @after = layers();
create('c2', 'tmpl');
is(scalar(layers()), scalar(@after), 'copy of a frozen template adds no layer');
is(contents('c2'), $tmpl, 'second copy has the template\'s contents');
my $rfn = $node->safe_psql('tmpl', "SELECT pg_relation_filenode('t')");
my $frozen = (layers())[-2];
my ($tmpl_oid, $c2_oid) = (dboid('tmpl'), dboid('c2'));
is(inode($frozen, $c2_oid, $rfn), inode($frozen, $tmpl_oid, $rfn),
	'copy\'s file is the template\'s');
my $top = (layers())[-1];
ok(-e $node->data_dir . "/pg_pagestore/$top/1663_${c2_oid}_${gone_rfn}_main.map",
	'copy drops what the template dropped since the freeze');

# A template with much written since the last freeze is frozen instead
$node->safe_psql('postgres', 'CREATE DATABASE big');
$node->safe_psql('big',
	"CREATE TABLE t AS SELECT g AS a, repeat('x', 500) AS b FROM generate_series(1, 50000) g");
@before = layers();
create('big_copy', 'big');
is(scalar(layers()), scalar(@before) + 1, 'copy of a template with much new data freezes');
is($node->safe_psql('big_copy', 'SELECT count(*) FROM t'), '50000', 'copy of the large template');

# Each database's writes stay its own
$node->safe_psql('c1', "UPDATE t SET b = 'c1' WHERE a <= 100; INSERT INTO t VALUES (0, 'c1')");
$node->safe_psql('tmpl', "UPDATE t SET b = 'tmpl' WHERE a > 19900");
is(contents('c2'), $tmpl, 'writes to the template and another copy don\'t show in a copy');
is($node->safe_psql('tmpl', "SELECT count(*) FROM t WHERE b = 'c1'"),
	'0', 'a copy\'s writes don\'t show in the template');
is($node->safe_psql('c1', "SELECT count(*) FROM t WHERE b = 'c1'"),
	'101', 'a copy sees its own writes');
check_indexes('c1', 'copy after writes');

# The template is in use while someone is connected to it
my $session = $node->background_psql('tmpl');
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	'CREATE DATABASE busy TEMPLATE tmpl STRATEGY = SNAPSHOT');
like($stderr, qr/is being accessed by other users/,
	'a template in use can\'t be copied');
$session->quit;

# Copy the template again, now with writes both in frozen layers and in the
# top layer, which the copy copies, and see it survive a crash right after
$tmpl = contents('tmpl');
create('c3', 'tmpl');
$node->stop('immediate');
$node->start;
is(contents('c3'), $tmpl, 'copy survives a crash right after its creation');
check_indexes('c3', 'copy after crash');
is($node->safe_psql('c3', 'SELECT count(*) FROM u'),
	'0', 'copy\'s unlogged table is empty after a crash');

# Merging writes a fork that copies share once, and gives them the result
$node->safe_psql('tmpl', "UPDATE t SET b = 'again' WHERE a <= 10");
create('c4', 'tmpl');
create('c5', 'tmpl');
$tmpl = contents('tmpl');
my $merged = $node->safe_psql('postgres',
	'SELECT merged_layers FROM pg_pagestore_merge(1)');
ok($merged > 0, "merged $merged pairs of layers");
$frozen = (layers())[-2];
my ($c4_oid, $c5_oid) = (dboid('c4'), dboid('c5'));
is(inode($frozen, $c4_oid, $rfn), inode($frozen, $c5_oid, $rfn),
	'copies share the merged file');
is(contents('c4'), $tmpl, 'copy after merging');
is(contents('c5'), $tmpl, 'other copy after merging');
check_indexes('c4', 'copy after merging');
is(contents('c2'), $orig, 'older copy after merging');

# Dropping the template leaves its copies alone
$node->safe_psql('postgres', 'DROP DATABASE tmpl');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', "SELECT pg_branch('" . $node->basedir . "/freeze', false)");
$node->safe_psql('postgres', 'SELECT * FROM pg_pagestore_reclaim()');
is(contents('c4'), $tmpl, 'copy after its template was dropped and reclaimed');
check_indexes('c4', 'copy after its template was dropped');

# While a dropped database's files remain, its OID isn't used again
$node->safe_psql('postgres', 'DROP DATABASE c5');
($ret, $stdout, $stderr) = $node->psql('postgres',
	"CREATE DATABASE c6 OID = $c5_oid TEMPLATE c4 STRATEGY = SNAPSHOT");
like($stderr, qr/data directory with the specified OID $c5_oid already exists/,
	'a dropped database\'s OID waits for its files to go');

# Replay of a copy whose files are gone copies the template again.  Drop
# the copy, freeze and reclaim it away, and crash before a checkpoint.
# Reclaiming takes two passes: the files, then the maps saying the forks
# were dropped.
$node->safe_psql('postgres', 'CHECKPOINT');
create('c7', 'c4');
my $c7_oid = dboid('c7');
$node->safe_psql('postgres', 'DROP DATABASE c7');
$node->safe_psql('postgres', "SELECT pg_branch('" . $node->basedir . "/freeze2', false)");
$node->safe_psql('postgres', 'SELECT * FROM pg_pagestore_reclaim()');
$node->safe_psql('postgres', 'SELECT * FROM pg_pagestore_reclaim()');
my @left = glob($node->data_dir . "/pg_pagestore/*/1663_${c7_oid}_*");
is(scalar(@left), 0, 'reclaiming removes all files of a dropped copy');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_database WHERE datname = 'c7'"),
	'0', 'dropped copy stays dropped after replaying its creation');
is(contents('c4'), $tmpl, 'its template after replay');

# The files of the database dropped earlier are gone too, and its OID free
create_with_oid('c6', 'c4', $c5_oid);
is(contents('c6'), $tmpl, 'copy made with a freed OID');

# A branch has the copies
my $branch = PostgreSQL::Test::Cluster->new('branch');
$node->safe_psql('postgres', "SELECT pg_branch('" . $branch->data_dir . "')");
$branch->append_conf('postgresql.conf', 'port = ' . $branch->port);
$branch->start;
is($branch->safe_psql('c4', 'SELECT count(*) FROM t'),
	$node->safe_psql('c4', 'SELECT count(*) FROM t'), 'branch has the copies');
$branch->stop;

# What the strategy can't do
my $ts = PostgreSQL::Test::Utils::tempdir_short();
$node->safe_psql('postgres', "CREATE TABLESPACE ts LOCATION '$ts'");
($ret, $stdout, $stderr) = $node->psql('postgres',
	'CREATE DATABASE c8 TEMPLATE c4 STRATEGY = SNAPSHOT TABLESPACE ts');
like($stderr, qr/requires both databases to be in tablespace "pg_default"/,
	'new database outside pg_default');
$node->safe_psql('c2', 'CREATE TABLE elsewhere (a int) TABLESPACE ts');
($ret, $stdout, $stderr) = $node->psql('postgres',
	'CREATE DATABASE c8 TEMPLATE c2 STRATEGY = SNAPSHOT');
like($stderr, qr/requires the template's relations to be in tablespace "pg_default"/,
	'template with relations outside pg_default');

my $md = PostgreSQL::Test::Cluster->new('md');
$md->init;
$md->start;
($ret, $stdout, $stderr) = $md->psql('postgres',
	'CREATE DATABASE c TEMPLATE template1 STRATEGY = SNAPSHOT');
like($stderr, qr/requires a cluster that uses the page store/,
	'cluster without the page store');
$md->stop;

$node->stop;
done_testing();
