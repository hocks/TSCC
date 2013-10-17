#!/usr/bin/perl -w
#
# Run some tests.
#
# Copyright (C) 2000-6 Pete Wyckoff <pw@osc.edu>
#
# $Id: runtests.pl 388 2006-11-27 17:09:48Z pw $
#
use strict qw(subs refs);
use English;

#
# If "qsub" is not your qsub program, change $qsub to include the explicit
# path.  You can also add other qsub options here too.
#
$qsub = "qsub";

#
# If the backend nodes don't mount this build directory, or they mount
# it under a different name, you'll have to change $dir to suit them.
# If the directory is not available, copy the mpiexec and hello codes
# somewhere the compute nodes can get to them, and point $dir there.
#
$dir = `pwd`; chomp($dir);

#
# This script assumes that it will be able to get $available_nodes nodes and
# $smpsize processors per node from pbs during the testing phase.  Change
# these numbers if this won't work for your site.
#
$available_nodes = 4;
$smpsize = 2;

#
# If your PBS configuration has time-shared nodes (generally only for largish
# SMPs), you should set this to 1 which tells the script to request, e.g.
# "ncpus=2" instead of "nodes=4:ppn=2".  In this case, $available_nodes above
# is ignored, only 1 node is used.  Node properties (below) are also ignored
# when this is on.
#
#$use_ncpus = 1;

#
# Any special node properties?  This string will get appended to the
# nodes line, separated by a colon, e.g.:  -l nodes=2:ppn=2:amd
# If your PBS scheduler is clever and tries to run single-node jobs
# on hardware that does not have a fancy network card, the job may fail
# when it tries to open the network card; add the right property here.
#
#$nodeprops = "amd";
#$nodeprops = "myrinet";
#$nodeprops = "ib";
#$nodeprops = "gige";

#
# If, for testing purposes, you want to pass some arguments to mpiexec
# every time, put them here.  Perhaps specify a communication library
# that is different from the compiled-in default.
#
#$mpcommargs = "--comm=pmi";

#
# Similarly, if you need to set up your environment or load modules or
# whatever, do it in this line that will be run immediately before the
# mpiexec test line, once in every batch script.
#
#$mpenvsetup = "mod load mpich-p4";

#
# If you know that it is not possible in your configuration to
# use your default shell, so you want a permanent "#PBS -S /bin/sh"
# line in every script, put it here.  This also disables the
# various shell tests.  Set it to "" to use the user's default shell
# and still disable testings all the various shells.
#
#$fixed_shell = "/bin/sh";
$fixed_shell = "";

#
# No need to change below here, hopefully.
#

$testlines = 5;          # number of lines to feed to hello across stdin
$sleep_interval = 1.0;   # seconds between dots during wait
# initialize possibly empty variables
$nodeprops = ""   unless defined($nodeprops);
$mpcommargs = ""  unless defined($mpcommargs);
$use_ncpus = 0    unless defined($use_ncpus);

#
# Grep through an output file.  Set this flag before the run if you
# only want to count the lines.  Problems with pbs_demux mixing up the
# output prevents testing the actual contents of each line reliably in
# the case of -nostdout.
#
$lookhello_count_only = 0;
sub lookhello($$$) {
    my ($file, $lines_expected, $file_description) = @_;
    my $lines = 0;

    if (!open(OUT, $file)) {
	print "Missing $file_description output file: $file.\n";
	return;
    }
    while (<OUT>) {
	++$lines;
	if ($lookhello_count_only) {
	    next;
	}
	if (!/^hello /) {
	    if (defined($ignore_regex) && /$ignore_regex/) {
		--$lines;  # explicitly requested this line be ignored
	    } else {
		print "File $file: unexpected line: $_";
	    }
	}
    }
    close(OUT);
    if ($lines != $lines_expected) {
	print "File $file: got $lines lines, expected $lines_expected.\n"
    }
}

#
# Run one test.  Verify results.
# args:
#   args to mpexec
#   args to hello
#   lines in mpiexec joined stdout/err
#   lines in pbs joined stdout/err
#
$iter = 1;
sub run($$$$) {
    my ($mpargs, $helloargs, $expectmp, $expectpbs) = @_;
    ## debugging
    #$mpargs .= " -v";

    my $suffix = sprintf("%d.%02s", $$, $iter);
    my $scrfile = "testqs.$suffix";
    my $outfile = "testqo.$suffix";
    my $helloout = "testho.$suffix";

    open(TMP, ">$scrfile");
    print TMP "#!/bin/sh\n";
    my $str;
    if ($use_ncpus > 0) {
	$str = "#PBS -l ncpus=$smpsize";  # nodes is always 1
    } else {
	$str = "#PBS -l nodes=$nodes:ppn=$smpsize";
	if (length($nodeprops)) {
	    $str .= ":$nodeprops";
	}
    }
    print TMP "$str\n";
    print TMP "#PBS -l walltime=5:00\n";
    print TMP "#PBS -l cput=", 5 * $nodes * $smpsize, ":00\n";
    print TMP "#PBS -j oe\n";
    print TMP "#PBS -o $outfile\n";
    if (length($shell)) {
	print TMP "#PBS -S $shell\n";
    }
    print TMP "cd $dir\n";
    if (defined($mpenvsetup) && $mpenvsetup =~ /./) {
	print TMP "$mpenvsetup\n";
    }
    if (defined($stdin_no_pipe)) {
	print TMP $stdin_no_pipe;
    } elsif (defined($stdin_gets)) {
	print TMP "echo $stdin_gets | ";
    } else {
	# What is a good way to do this?  brooks@aero.org pointed out a
	# nice way to use perl, but it may not be on the compute nodes.
	# echo -e is not on bsd.  seq, jot are non-standard.
	print TMP "echo '[1+pd$testlines>x]sx0lxxq' | dc | ";
    }
    print TMP "./mpiexec $mpcommargs $mpargs";
    if ($mpargs !~ /-config/) {
	print TMP " hello $helloargs";
    }
    print TMP " > $helloout 2>&1\n";
    close(TMP);

    my $qsub_res;
    my $qsub_errfile = "/tmp/mpiexec-runtests-qsub.err";
    for (;;) {
	unlink($qsub_errfile);
	$qsub_res = `sh -c '$qsub $scrfile 2>$qsub_errfile'`;
	if (-s $qsub_errfile) {
	    print "Runtests.pl: qsub failed: ";
	    system("cat $qsub_errfile");
	    unlink($qsub_errfile);
	    print "Runtests.pl: trying again in 1 second.\n";
	    sleep(1);
	    next;
	}
	unlink($qsub_errfile);
	last;
    }

    # wait for the results
    $qsub_res =~ s/\..*\n//;
    print "$qsub_res to $outfile mpiexec";
    if (length($mpcommargs)) {
	print " $mpcommargs";
    }
    if (length($mpargs)) {
	print " $mpargs";
    }
    print " hello";
    if (length($helloargs)) {
	print " $helloargs";
    }
    if (length($shell)) {
	print " shell=\"$shell\"";
    }
    print " ";
    for (; ! -f $outfile;) {
	print ".";
	select(undef, undef, undef, $sleep_interval);
    }
    print "\n";
    # wait a bit for the file contents to completely appear, NFS or copy
    select(undef, undef, undef, 2 * $sleep_interval);

    lookhello($helloout, $expectmp, "mpiexec stdout");
    lookhello($outfile, $expectpbs, "PBS -o output");

    # inc iter for next
    ++$iter;
    # let gm and pbs catch and clean up
    sleep(1);
}

sub numproc_tests(@)
{
    my @testlist = @_;

    # -n <numproc>
    for my $i (@testlist) {
	run("-n $i", "", $i, 0);
    }
}

sub pernode_nolocal_tests()
{
    if ($smpsize > 1) {
	run("-pernode", "", $nodes, 0);
    }
    if ($nodes > 1) {
	run("-nolocal", "", ($nodes - 1) * $smpsize, 0);
	run("-nolocal -pernode", "", $nodes - 1, 0);
    }
}

#
# stdio tests
#
sub stdio_tests($)
{
    my ($count) = @_;

    # 1..6 hello should ignore the stdin
    run("", "", $count, 0);
    run("-nostdin", "", $count, 0);
    run("-allstdin", "", $count, 0);
    $lookhello_count_only = 1;
    run("-nostdout", "", 0, $count);
    run("-nostdin -nostdout", "", 0, $count);
    run("-allstdin -nostdout", "", 0, $count);
    $lookhello_count_only = 0;

    # 7..12 hello will try to cat the stdin
    run("", "-l", $count + $testlines, 0);
    run("-nostdin", "-l", $count, 0);
    run("-allstdin", "-l", $count + $count * $testlines, 0);
    $lookhello_count_only = 1;
    run("-nostdout", "-l", 0, $count + $testlines);
    run("-nostdout -nostdin", "-l", 0, $count);
    run("-nostdout -allstdin", "-l", 0, $count + $count * $testlines);
    $lookhello_count_only = 0;

    # 13..18 things should work on stderr too
    run("", "-stderr", $count, 0);
    run("-nostdin", "-stderr", $count, 0);
    run("-allstdin", "-stderr", $count, 0);
    $lookhello_count_only = 1;
    run("-nostdout", "-stderr", 0, $count);
    run("-nostdin -nostdout", "-stderr", 0, $count);
    run("-allstdin -nostdout", "-stderr", 0, $count);
    $lookhello_count_only = 0;

    # 19..24 sleepy hello is slow to exit after it closes all stdio
    run("", "-sleep", $count, 0);
    run("-nostdin", "-sleep", $count, 0);
    run("-allstdin", "-sleep", $count, 0);
    $lookhello_count_only = 1;
    run("-nostdout", "-sleep", 0, $count);
    run("-nostdout -nostdin", "-sleep", 0, $count);
    run("-nostdout -allstdin", "-sleep", 0, $count);
    $lookhello_count_only = 0;

    # closing stdin should work, even if no -nostdin (though silly)
    # Note if you configured your torque with --enable-shell-pipe
    # (the default), bash will exit when this closes its command
    # stream.  If there's no .ho file, that's why.  Use --enable-shell-argv
    # instead.
    $stdin_no_pipe = "exec 0<&-\n";
    run("", "-l", $count, 0);
    undef($stdin_no_pipe);
}

#
# Test what happens when a process dies with segv.
#
sub segv_tests($@)
{
    my $count = shift @_;
    my @testlist = @_;

    # The various MPIs say different things as processes die oddly that we try
    # to catch and ignore here.  Mpich/p4 catches the segv and does exit(1)
    # instead, meaning the others may or may not exit properly.  Dumb.  Worse
    # yet, each task tries to call up all the others and sleeps 2 sec between
    # failed attempts.  That really scales, yeah.  Could add -kill here to avoid
    # that issue, although it covers up the mpich problem.

    print "Note: mpich1/p4 catches SIGSEGV then tries to contact all other\n";
    print "processes, with a 2 second delay after each.  If you are using\n";
    print "that MPI implementation, these tests could hang until the job\n";
    print "walltime (5 min) is reached.\n";

    # the selected segvs after 2 sec, rest sleep 6 sec
    $ignore_regex = "(died with signal|exited with status|p4_error|net_send)";
    for my $i (@testlist) {
	run("", "-sleep -segv " . ($i-1), $count, 0);
    }

    # all do segv before MPI_Init
    $ignore_regex = "(died with signal|exited before completing MPI startup|were never spawned)";
    run("", "-segvearly", 0, 0);

    undef($ignore_regex);
}

#
# Test when a process calls MPI_Abort().  Note that mpich2 (using PMI) does
# not support abort.  The other tasks may hang in MPI_Finalize and these
# tests will exit only after the batch system time limit.
#
sub abort_tests($@)
{
    my $count = shift @_;
    my @testlist = @_;

    print "Note: MPI_Abort is not implemented properly in mpich2.  If you\n";
    print "are using that MPI implementation, these tests will hang until\n";
    print "the job walltime (5 min) is reached.\n";

    # the given task calls MPI_Abort after 2 sec, while the rest sleep 6 sec
    $ignore_regex = "(died with signal|exited with status|p4_error|net_send"
      . "|MPI[_ ]Abort|Aborting program|aborting job)";
    for my $i (@testlist) {
	run("", "-sleep -abort " . ($i-1), $count, 0);
    }
}

#
# Check exit value returned to environment.
#
sub exitval_tests($)
{
    my $count = shift @_;

    for (my $i=0; $i<=2; $i++) {
	if ($i > 0) {
	    $ignore_regex = "exited with status $i\\\.";
	}
	run("", "-exitval $i", $count, 0);
	undef($ignore_regex);
    }
}

#
# Hostname transformation.
#
sub transform_hostname_tests($)
{
    my $count = shift @_;

    run("--transform-hostname=s/hiya/hiya/", "", $count, 0);
    run("--transform-hostname-program=cat", "", $count, 0);
}

#
# Test use of config file, once from a file and once from stdin.
# Builds the config file or stdin string, then calls run() to do
# the dirty work and verify the results.
#
sub config_tests(@)
{
    my @testlist = @_;

    for my $i (@testlist) {
	my $s = sprintf("testc.%d.%02s", $$, $iter);
	open(TMP, ">$s");
	print TMP "# mpiexec config\n";
	print TMP "-n $i : hello\n";
	close(TMP);
	run("-config $s", "", $i, 0);
    }
    for my $i (@testlist) {
	$stdin_gets = "\"-n $i : hello\"";
	run("-config=- -nostdin", "", $i, 0);
	undef($stdin_gets);
    }

    # Test constraint handling too:  more or fewer lines in config
    # file than are available, with or without -np on command line.
    # Just test max size in list.
    my $i = $testlist[$#testlist];
    my $j;

    # config file specifies more than available, with and without -np
    $j = $i + 1;
    $stdin_gets = "\"-n $j : hello\"";
    run("-config=- -nostdin", "", $i, 0);
    run("-config=- -nostdin -np $i", "", $i, 0);

    # config file specifies fewer than available, with and without -np
    if ($i > 1) {
	$j = $i - 1;
	$stdin_gets = "\"-n $j : hello\"";
	run("-config=- -nostdin", "", $j, 0);
	# should error, asked for $i but config file only gave $i-1
	$ignore_regex = "(specifies $i processors|only matched $j\\\.)";
	run("-config=- -nostdin -np $i", "", 0, 0);
	undef($ignore_regex);
    }

    # done
    undef($stdin_gets);
}

#
# Test each shell, make sure the "cd" in the mpiexec startup line works,
# and test passing a single quote.
#
# Some shells are just unsuitable for use non-interactively.
# In particular, csh and tcsh here complain about "no access
# to tty (Bad file descriptor)", but work anyway.
#
sub shell_test()
{
    $nodes = 1;
    my $count = $nodes * $smpsize;
    my @shells;
    print "Testing the various shells on your system.\n";
    if (open(SHELLS, "/etc/shells")) {
	while (<SHELLS>) {
	    chomp;
	    if (-x $_) {
		push @shells, $_;
	    }
	}
	close(SHELLS);
    } else {
	# reasonable defaults?
	for ( "/bin/sh", "/bin/csh" ) {
	    if (-x $_) {
		push @shells, $_;
	    }
	}
    }
    # modify global $shell for each test
    for $shell (@shells) {
	print "shell is $shell\n";
	# these escapes in perl yield "don\'doit" in file
	run("", "don\\\'tdoit", $count, 0);
    }
}

#
# main()
#
$| = 1;  # see output immediately

if (not -x "hello") {
    print "No executable \"hello\".  You might \"make hello\" first.\n";
    exit(1);
}

# Common user error:  PBS is not happy running jobs for root.
if ($EUID == 0) {
    print "PBS will not run jobs for root.",
      "  Run this script as a normal user.\n";
    exit(1);
}

# what shell
if (defined($fixed_shell) && $fixed_shell =~ /./) {
    $shell = $fixed_shell;
} else {
    $shell = "";  # use user default
}

# test max nodes and just one node
if ($use_ncpus > 0) {
    $available_nodes = 1;  # no multi-node for time-shared
}
@nodeset = ($available_nodes);
if ($available_nodes > 1) {  # test nodes=1 case too
    push @nodeset, 1;
}

foreach $nodes (@nodeset) {
    print "Testing $nodes node", ($nodes > 1 ? "s" : ""),
      " with SMP size $smpsize.\n";

    # total number of processes
    my $count = $nodes * $smpsize;

    #
    # Only test some subset of all possible combinations.  Just beyond
    # the SMP size, then the full size.
    #
    my @testlist;
    my $max = $smpsize + 1;
    if ($max > $count) {
	@testlist = (1..$count);
    } else {
	@testlist = (1..$max,$count);
    }

    numproc_tests(@testlist);
    pernode_nolocal_tests();
    stdio_tests($count);
    segv_tests($count, @testlist);
    abort_tests($count, @testlist);
    exitval_tests($count);
    transform_hostname_tests($count);
    config_tests(@testlist);
}

if (!defined($fixed_shell)) {
    shell_test();
}

exit 0;

