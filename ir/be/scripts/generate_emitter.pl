#!/usr/bin/perl -w

# This script generates C code which emits assembler code for the
# assembler ir nodes. It takes a "emit" key from the node specification
# and substitutes lines starting with . with a corresponding fprintf().
# Creation: 2005/11/07
# $Id$

use strict;
use Data::Dumper;

my $specfile   = $ARGV[0];
my $target_dir = $ARGV[1];

our $arch;
our %nodes;

# include spec file

my $return;

no strict "subs";
unless ($return = do $specfile) {
  warn "couldn't parse $specfile: $@" if $@;
  warn "couldn't do $specfile: $!"    unless defined $return;
  warn "couldn't run $specfile"       unless $return;
}
use strict "subs";

my $target_c = $target_dir."/gen_".$arch."_emitter.c";
my $target_h = $target_dir."/gen_".$arch."_emitter.h";

# stacks for output
my @obst_func;   # stack for the emit functions
my @obst_header;  # stack for the function prototypes

my $line;

foreach my $op (keys(%nodes)) {
  my %n = %{ $nodes{"$op"} };

  # skip this node description if no emit information is available
  next if (!$n{"emit"} || length($n{"emit"}) < 1);

  $line = "void emit_".$arch."_".$op."(FILE *F, ir_node *n)";
  push(@obst_header, $line.";\n");
  push(@obst_func, $line." {\n");

  # check in/out register if needed
  if (exists($n{"check_inout"}) && $n{"check_inout"} == 1) {
    push(@obst_func, "  equalize_dest_src(F, n);\n\n");
  }

  my @emit = split(/\n/, $n{"emit"});

  foreach(@emit) {
    # substitute only lines, starting with a '.'
    if (/^(\d*)\.\s*/) {
      my @params;
      my $regkind;
      my $indent = "  "; # default indent is 2 spaces

      $indent = " " x $1 if ($1 && $1 > 0);
      # remove indent, dot and trailing spaces
      s/^\d*\.\s*//;
      # substitute all format parameter
      while (/%(([sd])(\d)|([co]))/) {
        if ($4 && $4 eq "c") {
          push(@params, "node_const_to_str(n)");
        }
        elsif ($4 && $4 eq "o") {
          push(@params, "node_offset_to_str(n)");
        }
        else {
          $regkind = ($2 eq "s" ? "source" : "dest");
          push(@params, "get_".$regkind."_reg_name(n, $3)");
        }
        s/%$1/%%\%s/;
      }
      my $parm = "";
      $parm = ", ".join(", ", @params) if (@params);
      push(@obst_func, $indent.'fprintf(F, "\t'.$_.'\n"'.$parm.');'."\n");
    }
    else {
      push(@obst_func, $_,"\n");
    }
  }
  push(@obst_func, "}\n\n");
}

open(OUT, ">$target_h") || die("Could not open $target_h, reason: $!\n");

my $creation_time = localtime(time());

my $tmp = uc($arch);

print OUT<<EOF;
#ifndef _GEN_$tmp\_EMITTER_H_
#define _GEN_$tmp\_EMITTER_H_

/**
 * Function prototypes for the emitter functions.
 * DO NOT EDIT THIS FILE, your changes will be lost.
 * Edit $specfile instead.
 * created by: $0 $specfile $target_dir
 * date:       $creation_time
 */

#include "irnode.h"

EOF

print OUT @obst_header;

print OUT "#endif /* _GEN_$tmp\_EMITTER_H_ */\n";

close(OUT);

open(OUT, ">$target_c") || die("Could not open $target_c, reason: $!\n");

$creation_time = localtime(time());

print OUT<<EOF;
/**
 * Generated functions to emit code for assembler ir nodes.
 * DO NOT EDIT THIS FILE, your changes will be lost.
 * Edit $specfile instead.
 * created by: $0 $specfile $target_dir
 * date:       $creation_time
 */

#include <stdio.h>

#include "irnode.h"
#include "gen_$arch\_emitter.h"
#include "$arch\_emitter.h"
#include "$arch\_new_nodes.h"

EOF

print OUT @obst_func;

close(OUT);
