# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the page store's top layer running out of fork entries or bitmap
# chunks: the query that needs one fails and the cluster carries on,
# freezing frees them, and the merger freezes before they run out.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('limits');
$node->init(extra => ['--pagestore']);
$node->append_conf(
	'postgresql.conf', q{
pagestore_max_forks = 1024
pagestore_auto_freeze = off
pagestore_max_frozen_layers = 0
checkpoint_timeout = 1h
});
$node->start;

# Files in the top layer, of one database or of all
sub top_files_of
{
	my ($dboid) = @_;
	my @stack = split ' ', slurp_file($node->data_dir . '/pg_pagestore/STACK');
	opendir(my $dh, $node->data_dir . "/pg_pagestore/$stack[-1]") or die;
	my @files = grep { !/^\./ && (!defined $dboid || /^\d+_${dboid}_/) } readdir($dh);
	closedir($dh);
	return scalar(@files);
}

sub top_files { return top_files_of(undef); }

is(top_files(), 0, 'initdb leaves the top layer empty');

sub restart_with
{
	my ($conf) = @_;
	$node->append_conf('postgresql.conf', $conf);
	$node->restart;
}

# A template, frozen, with a table big enough for writes below the base of
# its fork to need bitmap chunks
$node->safe_psql('postgres', 'CREATE DATABASE tmpl');
$node->safe_psql('tmpl',
	"CREATE TABLE b AS SELECT g AS a, repeat('x', 100) AS pad FROM generate_series(1, 100000) g"
);
$node->safe_psql('postgres', "SELECT pg_pagestore_freeze('tmpl')");

# Fork entries: tables made in copies, and the catalog pages they change, take
# entries once the checkpoint writes them out, until there are none left
restart_with("pagestore_max_bitmap_chunks = 16384");
my ($ret, $stdout, $stderr);
my $failed = 0;
foreach my $i (1 .. 50)
{
	$node->safe_psql('postgres',
		"CREATE DATABASE f$i TEMPLATE tmpl STRATEGY = SNAPSHOT");
	($ret, $stdout, $stderr) = $node->psql("f$i",
		q{DO $$ BEGIN FOR i IN 1..50 LOOP EXECUTE format('CREATE TABLE x%s (a int)', i); END LOOP; END $$});
	($ret, $stdout, $stderr) = $node->psql('postgres', 'CHECKPOINT')
	  if $ret == 0;
	if ($ret != 0)
	{
		$failed = $i;
		last;
	}
}
ok($failed, "running out of fork entries fails a query, at copy $failed");
like($stderr, qr/out of page store fork entries|checkpoint request failed/,
	'the query reports it');
like(slurp_file($node->logfile), qr/out of page store fork entries/,
	'the log says why');
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_database'),
	$node->safe_psql('f1', 'SELECT count(*) FROM pg_database'),
	'fork entries out: the cluster answers');
($ret, $stdout, $stderr) = $node->psql('postgres',
	'CREATE DATABASE wont TEMPLATE tmpl STRATEGY = SNAPSHOT');
pass('fork entries out: CREATE DATABASE returns' .
	  ($ret ? " an error: $stderr" : ''));
$node->safe_psql('postgres', 'SELECT pg_pagestore_freeze()');
$node->safe_psql('postgres', 'CHECKPOINT');
pass('after freezing, the checkpoint completes');

# Bitmap chunks: each copy updates the big table it shares with the template.
# A backend's write fails when there are none left; the checkpointer's
# freezes to make room instead, even without pagestore_auto_freeze, or a
# shutdown checkpoint could fail.
restart_with("pagestore_max_forks = 65536\npagestore_max_bitmap_chunks = 64");
$failed = 0;
foreach my $i (1 .. 100)
{
	$node->safe_psql('postgres',
		"CREATE DATABASE k$i TEMPLATE tmpl STRATEGY = SNAPSHOT");
	($ret, $stdout, $stderr) = $node->psql("k$i",
		'UPDATE b SET pad = pad WHERE a % 1000 = 0; CHECKPOINT');
	if ($ret != 0)
	{
		$failed = $i;
		last;
	}
}
if ($failed)
{
	like($stderr, qr/out of page store bitmap chunks/,
		"running out of bitmap chunks fails a write, at copy $failed");
}
else
{
	like(slurp_file($node->logfile),
		qr/checkpointer.*page store froze its top layer, which was full/,
		'the checkpointer freezes when it runs out of bitmap chunks');
}
is($node->safe_psql('k1', 'SELECT count(*) FROM b'),
	'100000', 'bitmap chunks out: the cluster answers');
$node->safe_psql('postgres', 'SELECT pg_pagestore_freeze()');
$node->safe_psql('postgres', 'CHECKPOINT');
pass('after freezing, the checkpoint completes');
my $last = $failed || 100;
is($node->safe_psql("k$last", "SELECT count(*) FROM b WHERE a % 1000 = 0"),
	'100', 'the last copy written to reads its table');

# With pagestore_auto_freeze, the merger freezes before either runs out
restart_with("pagestore_max_forks = 1024\npagestore_auto_freeze = on");
my $froze_before = () = slurp_file($node->logfile) =~ /froze its top layer/g;
foreach my $i (1 .. 100)
{
	$node->safe_psql('postgres',
		"CREATE DATABASE a$i TEMPLATE tmpl STRATEGY = SNAPSHOT");
	$node->safe_psql("a$i",
		'UPDATE b SET pad = pad WHERE a % 1000 = 0; CHECKPOINT');
}
pass('100 copies written to and checkpointed, with the auto freeze');
my $froze = () = slurp_file($node->logfile) =~ /froze its top layer/g;
ok($froze > $froze_before,
	'the merger froze the top layer ' . ($froze - $froze_before) . ' times');

# Copies of a template that isn't frozen copy what it wrote, and freeze
# rather than fill the table with it, also without pagestore_auto_freeze
restart_with("pagestore_auto_freeze = off");
$node->safe_psql('postgres', 'CREATE DATABASE fresh');
$node->safe_psql('fresh',
	q{DO $$ BEGIN FOR i IN 1..100 LOOP EXECUTE format('CREATE TABLE m%s (a int)', i); END LOOP; END $$});
foreach my $i (1 .. 30)
{
	$node->safe_psql('postgres',
		"CREATE DATABASE fc$i TEMPLATE fresh STRATEGY = SNAPSHOT");
}
is($node->safe_psql('fc30', "SELECT count(*) FROM pg_class WHERE relname LIKE 'm%'"),
	'100', '30 copies of a template that was not frozen');

# A template frozen with pg_pagestore_freeze() costs its copies nothing
$node->safe_psql('postgres', "SELECT pg_pagestore_freeze('tmpl')");
my $in_top = 0;
foreach my $i (1 .. 20)
{
	$node->safe_psql('postgres',
		"CREATE DATABASE frozen$i TEMPLATE tmpl STRATEGY = SNAPSHOT");
	$in_top += top_files_of(
		$node->safe_psql('postgres',
			"SELECT oid FROM pg_database WHERE datname = 'frozen$i'"));
}
is($in_top, 0, 'copies of a frozen template have no files in the top layer');

$node->stop;
done_testing();
