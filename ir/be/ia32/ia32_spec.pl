# Creation: 2005/10/19
# $Id$
# This is the specification for the ia32 assembler Firm-operations

use File::Basename;

$new_emit_syntax = 1;
my $myname = $0;

# the cpu architecture (ia32, ia64, mips, sparc, ppc, ...)
$arch = "ia32";

# The node description is done as a perl hash initializer with the
# following structure:
#
# %nodes = (
#
# <op-name> => {
#   op_flags  => "N|L|C|X|I|F|Y|H|c|K",
#   irn_flags => "R|N"
#   arity     => "0|1|2|3 ... |variable|dynamic|any",
#   state     => "floats|pinned|mem_pinned|exc_pinned",
#   args      => [
#                    { type => "type 1", name => "name 1" },
#                    { type => "type 2", name => "name 2" },
#                    ...
#                  ],
#   comment   => "any comment for constructor",
#   reg_req   => { in => [ "reg_class|register" ], out => [ "reg_class|register|in_rX" ] },
#   cmp_attr  => "c source code for comparing node attributes",
#   outs      => { "out1", "out2" } # optional, creates pn_op_out1, ... consts
#   ins       => { "in1", "in2" }   # optional, creates n_op_in1, ... consts
#   mode      => "mode_Iu"          # optional, predefines the mode
#   emit      => "emit code with templates",
#   attr      => "additional attribute arguments for constructor",
#   init_attr => "emit attribute initialization template",
#   rd_constructor => "c source code which constructs an ir_node",
#   hash_func => "name of the hash function for this operation",
#   latency   => "latency of this operation (can be float)"
#   attr_type => "name of the attribute struct",
#   modified_flags =>  [ "CF", ... ] # optional, list of modified flags
# },
#
# ... # (all nodes you need to describe)
#
# ); # close the %nodes initializer

# op_flags: flags for the operation, OPTIONAL (default is "N")
# the op_flags correspond to the firm irop_flags:
#   N   irop_flag_none
#   L   irop_flag_labeled
#   C   irop_flag_commutative
#   X   irop_flag_cfopcode
#   I   irop_flag_ip_cfopcode
#   F   irop_flag_fragile
#   Y   irop_flag_forking
#   H   irop_flag_highlevel
#   c   irop_flag_constlike
#   K   irop_flag_keep
#   NB  irop_flag_dump_noblock
#   NI  irop_flag_dump_noinput
#
# irn_flags: special node flags, OPTIONAL (default is 0)
# following irn_flags are supported:
#   R   rematerializeable
#   N   not spillable
#
# state: state of the operation, OPTIONAL (default is "floats")
#
# arity: arity of the operation, MUST NOT BE OMITTED
#
# args:  the OPTIONAL arguments of the node constructor (debug, irg and block
#        are always the first 3 arguments and are always autmatically
#        created)
#        If this key is missing the following arguments will be created:
#        for i = 1 .. arity: ir_node *op_i
#        ir_mode *mode
#
# outs:  if a node defines more than one output, the names of the projections
#        nodes having outs having automatically the mode mode_T
#        example: [ "frame", "stack", "M" ]
#
# comment: OPTIONAL comment for the node constructor
#
# rd_constructor: for every operation there will be a
#      new_rd_<arch>_<op-name> function with the arguments from above
#      which creates the ir_node corresponding to the defined operation
#      you can either put the complete source code of this function here
#
#      This key is OPTIONAL. If omitted, the following constructor will
#      be created:
#      if (!op_<arch>_<op-name>) assert(0);
#      for i = 1 to arity
#         set in[i] = op_i
#      done
#      res = new_ir_node(db, irg, block, op_<arch>_<op-name>, mode, arity, in)
#      return res
#
# NOTE: rd_constructor and args are only optional if and only if arity is 0,1,2 or 3
#

# register types:
#   0 - no special type
#   1 - caller save (register must be saved by the caller of a function)
#   2 - callee save (register must be saved by the called function)
#   4 - ignore (do not automatically assign this register)
#   8 - emitter can choose an arbitrary register of this class
#  16 - the register is a virtual one
#  32 - register represents a state
# NOTE: Last entry of each class is the largest Firm-Mode a register can hold
%reg_classes = (
	gp => [
		{ name => "edx", type => 1 },
		{ name => "ecx", type => 1 },
		{ name => "eax", type => 1 },
		{ name => "ebx", type => 2 },
		{ name => "esi", type => 2 },
		{ name => "edi", type => 2 },
		{ name => "ebp", type => 2 },
		{ name => "esp", type => 4 },
		{ name => "gp_NOREG", type => 4 | 8 | 16 }, # we need a dummy register for NoReg nodes
		{ name => "gp_UKNWN", type => 4 | 8 | 16 },  # we need a dummy register for Unknown nodes
		{ mode => "mode_Iu" }
	],
	mmx => [
		{ name => "mm0", type => 4 },
		{ name => "mm1", type => 4 },
		{ name => "mm2", type => 4 },
		{ name => "mm3", type => 4 },
		{ name => "mm4", type => 4 },
		{ name => "mm5", type => 4 },
		{ name => "mm6", type => 4 },
		{ name => "mm7", type => 4 },
		{ mode => "mode_E", flags => "manual_ra" }
	],
	xmm => [
		{ name => "xmm0", type => 1 },
		{ name => "xmm1", type => 1 },
		{ name => "xmm2", type => 1 },
		{ name => "xmm3", type => 1 },
		{ name => "xmm4", type => 1 },
		{ name => "xmm5", type => 1 },
		{ name => "xmm6", type => 1 },
		{ name => "xmm7", type => 1 },
		{ name => "xmm_NOREG", type => 4 | 16 },     # we need a dummy register for NoReg nodes
		{ name => "xmm_UKNWN", type => 4 | 8 | 16},  # we need a dummy register for Unknown nodes
		{ mode => "mode_E" }
	],
	vfp => [
		{ name => "vf0", type => 1 },
		{ name => "vf1", type => 1 },
		{ name => "vf2", type => 1 },
		{ name => "vf3", type => 1 },
		{ name => "vf4", type => 1 },
		{ name => "vf5", type => 1 },
		{ name => "vf6", type => 1 },
		{ name => "vf7", type => 1 },
		{ name => "vfp_NOREG", type => 4 | 8 | 16 }, # we need a dummy register for NoReg nodes
		{ name => "vfp_UKNWN", type => 4 | 8 | 16 },  # we need a dummy register for Unknown nodes
		{ mode => "mode_E" }
	],
	st => [
		{ name => "st0", realname => "st",    type => 4 },
		{ name => "st1", realname => "st(1)", type => 4 },
		{ name => "st2", realname => "st(2)", type => 4 },
		{ name => "st3", realname => "st(3)", type => 4 },
		{ name => "st4", realname => "st(4)", type => 4 },
		{ name => "st5", realname => "st(5)", type => 4 },
		{ name => "st6", realname => "st(6)", type => 4 },
		{ name => "st7", realname => "st(7)", type => 4 },
		{ mode => "mode_E", flags => "manual_ra" }
	],
	fp_cw => [	# the floating point control word
		{ name => "fpcw", type => 4|32 },
		{ mode => "mode_fpcw", flags => "manual_ra|state" }
	],
	flags => [
		{ name => "eflags", type => 0 },
		{ mode => "mode_Iu", flags => "manual_ra" }
	],
); # %reg_classes

%cpu = (
	GP     => [ 1, "GP_EAX", "GP_EBX", "GP_ECX", "GP_EDX", "GP_ESI", "GP_EDI", "GP_EBP" ],
	SSE    => [ 1, "SSE_XMM0", "SSE_XMM1", "SSE_XMM2", "SSE_XMM3", "SSE_XMM4", "SSE_XMM5", "SSE_XMM6", "SSE_XMM7" ],
	VFP    => [ 1, "VFP_VF0", "VFP_VF1", "VFP_VF2", "VFP_VF3", "VFP_VF4", "VFP_VF5", "VFP_VF6", "VFP_VF7" ],
	BRANCH => [ 1, "BRANCH1", "BRANCH2" ],
); # %cpu

%vliw = (
	bundle_size       => 1,
	bundels_per_cycle => 1
); # vliw

%emit_templates = (
	S0 => "${arch}_emit_source_register(node, 0);",
	S1 => "${arch}_emit_source_register(node, 1);",
	S2 => "${arch}_emit_source_register(node, 2);",
	S3 => "${arch}_emit_source_register(node, 3);",
	SB0 => "${arch}_emit_8bit_source_register_or_immediate(node, 0);",
	SB1 => "${arch}_emit_8bit_source_register_or_immediate(node, 1);",
	SB2 => "${arch}_emit_8bit_source_register_or_immediate(node, 2);",
	SB3 => "${arch}_emit_8bit_source_register_or_immediate(node, 3);",
	SH0 => "${arch}_emit_8bit_high_source_register(node, 0);",
	SS0 => "${arch}_emit_16bit_source_register_or_immediate(node, 0);",
	SI0 => "${arch}_emit_source_register_or_immediate(node, 0);",
	SI1 => "${arch}_emit_source_register_or_immediate(node, 1);",
	SI3 => "${arch}_emit_source_register_or_immediate(node, 3);",
	D0 => "${arch}_emit_dest_register(node, 0);",
	D1 => "${arch}_emit_dest_register(node, 1);",
	DS0 => "${arch}_emit_dest_register_size(node, 0);",
	DB0 => "${arch}_emit_8bit_dest_register(node, 0);",
	X0 => "${arch}_emit_x87_register(node, 0);",
	X1 => "${arch}_emit_x87_register(node, 1);",
	EX => "${arch}_emit_extend_suffix(node);",
	M  => "${arch}_emit_mode_suffix(node);",
	XM => "${arch}_emit_x87_mode_suffix(node);",
	XXM => "${arch}_emit_xmm_mode_suffix(node);",
	XSD => "${arch}_emit_xmm_mode_suffix_s(node);",
	AM => "${arch}_emit_am(node);",
	unop3 => "${arch}_emit_unop(node, n_ia32_unary_op);",
	unop4 => "${arch}_emit_unop(node, n_ia32_binary_right);",
	binop => "${arch}_emit_binop(node);",
	x87_binop => "${arch}_emit_x87_binop(node);",
	CMP0  => "${arch}_emit_cmp_suffix_node(node, 0);",
	CMP3  => "${arch}_emit_cmp_suffix_node(node, 3);",
);

#--------------------------------------------------#
#                        _                         #
#                       (_)                        #
#  _ __   _____      __  _ _ __    ___  _ __  ___  #
# | '_ \ / _ \ \ /\ / / | | '__|  / _ \| '_ \/ __| #
# | | | |  __/\ V  V /  | | |    | (_) | |_) \__ \ #
# |_| |_|\___| \_/\_/   |_|_|     \___/| .__/|___/ #
#                                      | |         #
#                                      |_|         #
#--------------------------------------------------#

$default_op_attr_type = "ia32_op_attr_t";
$default_attr_type    = "ia32_attr_t";
$default_copy_attr    = "ia32_copy_attr";

sub ia32_custom_init_attr {
	my $node = shift;
	my $name = shift;
	my $res = "";

	if(defined($node->{modified_flags})) {
		$res .= "\tarch_irn_add_flags(res, arch_irn_flags_modify_flags);\n";
	}
	if(defined($node->{am})) {
		my $am = $node->{am};
		if($am eq "source,unary") {
			$res .= "\tset_ia32_am_support(res, ia32_am_unary);";
		} elsif($am eq "source,binary") {
			$res .= "\tset_ia32_am_support(res, ia32_am_binary);";
		} elsif($am eq "none") {
			# nothing to do
		} else {
			die("Invalid address mode '$am' specified on op $name");
		}
		if($am ne "none") {
			if($node->{state} ne "exc_pinned"
					and $node->{state} ne "pinned") {
				die("AM nodes must have pinned or AM pinned state ($name)");
			}
		}
	}
	return $res;
}
$custom_init_attr_func = \&ia32_custom_init_attr;

%init_attr = (
	ia32_asm_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_x87_attributes(res);".
		"\tinit_ia32_asm_attributes(res);",
	ia32_attr_t     =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);",
	ia32_call_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_call_attributes(res, pop, call_tp);",
	ia32_condcode_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_condcode_attributes(res, pnc);",
	ia32_copyb_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_copyb_attributes(res, size);",
	ia32_immediate_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_immediate_attributes(res, symconst, symconst_sign, no_pic_adjust, offset);",
	ia32_x87_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_x87_attributes(res);",
	ia32_climbframe_attr_t =>
		"\tinit_ia32_attributes(res, flags, in_reqs, exec_units, n_res);\n".
		"\tinit_ia32_climbframe_attributes(res, count);",
);

%compare_attr = (
	ia32_asm_attr_t       => "ia32_compare_asm_attr",
	ia32_attr_t            => "ia32_compare_nodes_attr",
	ia32_call_attr_t       => "ia32_compare_call_attr",
	ia32_condcode_attr_t   => "ia32_compare_condcode_attr",
	ia32_copyb_attr_t      => "ia32_compare_copyb_attr",
	ia32_immediate_attr_t  => "ia32_compare_immediate_attr",
	ia32_x87_attr_t        => "ia32_compare_x87_attr",
	ia32_climbframe_attr_t => "ia32_compare_climbframe_attr",
);

%operands = (
);

$mode_xmm           = "mode_E";
$mode_gp            = "mode_Iu";
$mode_flags         = "mode_Iu";
$mode_fpcw          = "mode_fpcw";
$status_flags       = [ "CF", "PF", "AF", "ZF", "SF", "OF" ];
$status_flags_wo_cf = [       "PF", "AF", "ZF", "SF", "OF" ];
$fpcw_flags         = [ "FP_IM", "FP_DM", "FP_ZM", "FP_OM", "FP_UM", "FP_PM",
                        "FP_PC0", "FP_PC1", "FP_RC0", "FP_RC1", "FP_X" ];

%nodes = (

Immediate => {
	state     => "pinned",
	op_flags  => "c",
	reg_req   => { out => [ "gp_NOREG:I" ] },
	attr      => "ir_entity *symconst, int symconst_sign, int no_pic_adjust, long offset",
	attr_type => "ia32_immediate_attr_t",
	hash_func => "ia32_hash_Immediate",
	latency   => 0,
	mode      => $mode_gp,
},

Asm => {
	mode      => "mode_T",
	arity     => "variable",
	out_arity => "variable",
	attr_type => "ia32_asm_attr_t",
	attr      => "ident *asm_text, const ia32_asm_reg_t *register_map",
	init_attr => "attr->asm_text = asm_text;\n".
	             "\tattr->register_map = register_map;\n",
	latency   => 10,
	modified_flags => $status_flags,
},

# "allocates" a free register
ProduceVal => {
	op_flags  => "c",
	irn_flags => "R",
	reg_req   => { out => [ "gp" ] },
	emit      => "",
	units     => [ ],
	latency   => 0,
	mode      => $mode_gp,
	cmp_attr  => "return 1;",
},

#-----------------------------------------------------------------#
#  _       _                                         _            #
# (_)     | |                                       | |           #
#  _ _ __ | |_ ___  __ _  ___ _ __   _ __   ___   __| | ___  ___  #
# | | '_ \| __/ _ \/ _` |/ _ \ '__| | '_ \ / _ \ / _` |/ _ \/ __| #
# | | | | | ||  __/ (_| |  __/ |    | | | | (_) | (_| |  __/\__ \ #
# |_|_| |_|\__\___|\__, |\___|_|    |_| |_|\___/ \__,_|\___||___/ #
#                   __/ |                                         #
#                  |___/                                          #
#-----------------------------------------------------------------#

# commutative operations

Add => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in  => [ "gp", "gp", "none", "gp", "gp" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	emit      => '. add%M %binop',
	am        => "source,binary",
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

AddMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => ". add%M %SI3, %AM",
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

AddMem8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => ". add%M %SB3, %AM",
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

Adc => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp", "flags" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right", "eflags" ],
	outs      => [ "res", "flags", "M" ],
	emit      => '. adc%M %binop',
	am        => "source,binary",
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

l_Add => {
	op_flags  => "C",
	reg_req   => { in => [ "none", "none" ], out => [ "none" ] },
	ins       => [ "left", "right" ],
},

l_Adc => {
	reg_req   => { in => [ "none", "none", "none" ], out => [ "none" ] },
	ins       => [ "left", "right", "eflags" ],
},

Mul => {
	# we should not rematrialize this node. It produces 2 results and has
	# very strict constraints
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax", "gp" ],
	               out => [ "eax", "flags", "none", "edx" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	emit      => '. mul%M %unop4',
	outs      => [ "res_low", "flags", "M", "res_high" ],
	am        => "source,binary",
	latency   => 10,
	units     => [ "GP" ],
	modified_flags => $status_flags
},

l_Mul => {
	# we should not rematrialize this node. It produces 2 results and has
	# very strict constraints
	op_flags  => "C",
	cmp_attr  => "return 1;",
	reg_req   => { in => [ "none", "none" ],
	               out => [ "none", "none", "none", "none" ] },
	ins       => [ "left", "right" ],
	outs      => [ "res_low", "flags", "M", "res_high" ],
},

IMul => {
	irn_flags => "R",
	state     => "exc_pinned",
	# TODO: adjust out requirements for the 3 operand form
	# (no need for should_be_same then)
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp" ],
		           out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	latency   => 5,
	units     => [ "GP" ],
	mode      => $mode_gp,
	modified_flags => $status_flags
},

IMul1OP => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax", "gp" ],
	               out => [ "eax", "flags", "none", "edx" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	emit      => '. imul%M %unop4',
	outs      => [ "res_low", "flags", "M", "res_high" ],
	am        => "source,binary",
	latency   => 5,
	units     => [ "GP" ],
	modified_flags => $status_flags
},

l_IMul => {
	op_flags  => "C",
	cmp_attr  => "return 1;",
	reg_req   => { in => [ "none", "none" ],
	               out => [ "none", "none", "none", "none" ] },
	ins       => [ "left", "right" ],
	outs      => [ "res_low", "flags", "M", "res_high" ],
},

And => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp" ],
		           out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	op_modes  => "commutative | am | immediate | mode_neutral",
	am        => "source,binary",
	emit      => '. and%M %binop',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

AndMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => '. and%M %SI3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

AndMem8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none",  "eax ebx ecx edx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => '. and%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

Or => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. or%M %binop',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

OrMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => '. or%M %SI3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

OrMem8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => '. or%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

Xor => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. xor%M %binop',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

Xor0 => {
	op_flags  => "c",
	irn_flags => "R",
	reg_req   => { out => [ "gp", "flags" ] },
	outs      => [ "res", "flags" ],
	emit      => ". xor%M %D0, %D0",
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

XorMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => '. xor%M %SI3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

XorMem8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	emit      => '. xor%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

# not commutative operations

Sub => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp" ],
	               out => [ "in_r4", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "minuend", "subtrahend" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. sub%M %binop',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

SubMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "subtrahend" ],
	emit      => '. sub%M %SI3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => 'mode_M',
	modified_flags => $status_flags
},

SubMem8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "subtrahend" ],
	emit      => '. sub%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => 'mode_M',
	modified_flags => $status_flags
},

Sbb => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp", "flags" ],
	               out => [ "in_r4 !in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "minuend", "subtrahend", "eflags" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. sbb%M %binop',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

Sbb0 => {
	irn_flags => "R",
	reg_req   => { in => [ "flags" ], out => [ "gp", "flags" ] },
	outs      => [ "res", "flags" ],
	emit      => ". sbb%M %D0, %D0",
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

l_Sub => {
	reg_req   => { in => [ "none", "none" ], out => [ "none" ] },
	ins       => [ "minuend", "subtrahend" ],
},

l_Sbb => {
	reg_req   => { in => [ "none", "none", "none" ], out => [ "none" ] },
	ins       => [ "minuend", "subtrahend", "eflags" ],
},

IDiv => {
	op_flags  => "F|L",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "eax", "edx" ],
	               out => [ "eax", "flags", "none", "edx", "none" ] },
	ins       => [ "base", "index", "mem", "divisor", "dividend_low", "dividend_high" ],
	outs      => [ "div_res", "flags", "M", "mod_res", "X_exc" ],
	am        => "source,unary",
	emit      => ". idiv%M %unop3",
	latency   => 25,
	units     => [ "GP" ],
	modified_flags => $status_flags
},

Div => {
	op_flags  => "F|L",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "eax", "edx" ],
	               out => [ "eax", "flags", "none", "edx", "none" ] },
	ins       => [ "base", "index", "mem", "divisor", "dividend_low", "dividend_high" ],
	outs      => [ "div_res", "flags", "M", "mod_res", "X_exc" ],
	am        => "source,unary",
	emit      => ". div%M %unop3",
	latency   => 25,
	units     => [ "GP" ],
	modified_flags => $status_flags
},

Shl => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "ecx" ],
	               out => [ "in_r1 !in_r2", "flags" ] },
	ins       => [ "val", "count" ],
	outs      => [ "res", "flags" ],
	emit      => '. shl%M %SB1, %S0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

ShlMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "ecx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "count" ],
	emit      => '. shl%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

l_ShlDep => {
	cmp_attr => "return 1;",
	reg_req  => { in => [ "none", "none", "none" ], out => [ "none" ] },
	ins      => [ "val", "count", "dep" ],
},

ShlD => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp", "ecx" ],
	               out => [ "in_r1 !in_r2 !in_r3", "flags" ] },
	ins       => [ "val_high", "val_low", "count" ],
	outs      => [ "res", "flags" ],
	emit      => ". shld%M %SB2, %S1, %D0",
	latency   => 6,
	units     => [ "GP" ],
	mode      => $mode_gp,
	modified_flags => $status_flags
},

l_ShlD => {
	cmp_attr  => "return 1;",
	reg_req  => { in => [ "none", "none", "none" ], out => [ "none" ] },
	ins       => [ "val_high", "val_low", "count" ],
},

Shr => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "ecx" ],
	               out => [ "in_r1 !in_r2", "flags" ] },
	ins       => [ "val", "count" ],
	outs      => [ "res", "flags" ],
	emit      => '. shr%M %SB1, %S0',
	units     => [ "GP" ],
	mode      => $mode_gp,
	latency   => 1,
	modified_flags => $status_flags
},

ShrMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "ecx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "count" ],
	emit      => '. shr%M %SB3, %AM',
	units     => [ "GP" ],
	mode      => "mode_M",
	latency   => 1,
	modified_flags => $status_flags
},

l_ShrDep => {
	cmp_attr  => "return 1;",
	reg_req   => { in => [ "none", "none", "none" ], out => [ "none" ] },
	ins       => [ "val", "count", "dep" ],
},

ShrD => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp", "ecx" ],
	               out => [ "in_r1 !in_r2 !in_r3", "flags" ] },
	ins       => [ "val_high", "val_low", "count" ],
	outs      => [ "res", "flags" ],
	emit      => ". shrd%M %SB2, %S1, %D0",
	latency   => 6,
	units     => [ "GP" ],
	mode      => $mode_gp,
	modified_flags => $status_flags
},

l_ShrD => {
	cmp_attr  => "return 1;",
	reg_req   => { in => [ "none", "none", "none" ], out => [ "none" ] },
	ins       => [ "val_high", "val_low", "count" ],
},

Sar => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "ecx" ],
	               out => [ "in_r1 !in_r2", "flags" ] },
	ins       => [ "val", "count" ],
	outs      => [ "res", "flags" ],
	emit      => '. sar%M %SB1, %S0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

SarMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "ecx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "count" ],
	emit      => '. sar%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

l_SarDep => {
	cmp_attr  => "return 1;",
	ins       => [ "val", "count", "dep" ],
	reg_req   => { in => [ "none", "none", "none" ], out => [ "none" ] },
},

Ror => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "ecx" ],
	               out => [ "in_r1 !in_r2", "flags" ] },
	ins       => [ "val", "count" ],
	outs      => [ "res", "flags" ],
	emit      => '. ror%M %SB1, %S0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

RorMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "ecx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "count" ],
	emit      => '. ror%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

Rol => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "ecx" ],
	               out => [ "in_r1 !in_r2", "flags" ] },
	ins       => [ "val", "count" ],
	outs      => [ "res", "flags" ],
	emit      => '. rol%M %SB1, %S0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

RolMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "ecx" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "count" ],
	emit      => '. rol%M %SB3, %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

# unary operations

Neg => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ],
	               out => [ "in_r1", "flags" ] },
	emit      => '. neg%M %S0',
	ins       => [ "val" ],
	outs      => [ "res", "flags" ],
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

NegMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	emit      => '. neg%M %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	modified_flags => $status_flags
},

Minus64Bit => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "in_r1", "in_r2" ] },
	outs      => [ "low_res", "high_res" ],
	units     => [ "GP" ],
	latency   => 3,
	modified_flags => $status_flags
},


Inc => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ],
	               out => [ "in_r1", "flags" ] },
	ins       => [ "val" ],
	outs      => [ "res", "flags" ],
	emit      => '. inc%M %S0',
	units     => [ "GP" ],
	mode      => $mode_gp,
	latency   => 1,
	modified_flags => $status_flags_wo_cf
},

IncMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	emit      => '. inc%M %AM',
	units     => [ "GP" ],
	mode      => "mode_M",
	latency   => 1,
	modified_flags => $status_flags_wo_cf
},

Dec => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ],
	               out => [ "in_r1", "flags" ] },
	ins       => [ "val" ],
	outs      => [ "res", "flags" ],
	emit      => '. dec%M %S0',
	units     => [ "GP" ],
	mode      => $mode_gp,
	latency   => 1,
	modified_flags => $status_flags_wo_cf
},

DecMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	emit      => '. dec%M %AM',
	units     => [ "GP" ],
	mode      => "mode_M",
	latency   => 1,
	modified_flags => $status_flags_wo_cf
},

Not => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ],
	               out => [ "in_r1", "flags" ] },
	ins       => [ "val" ],
	outs      => [ "res", "flags" ],
	emit      => '. not%M %S0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	# no flags modified
},

NotMem => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	emit      => '. not%M %AM',
	units     => [ "GP" ],
	latency   => 1,
	mode      => "mode_M",
	# no flags modified
},

Cmc => {
	reg_req => { in => [ "flags" ], out => [ "flags" ] },
	emit    => '.cmc',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_flags,
	modified_flags => $status_flags
},

Stc => {
	reg_req => { out => [ "flags" ] },
	emit    => '.stc',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_flags,
	modified_flags => $status_flags
},

# other operations

Cmp => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in  => [ "gp", "gp", "none", "gp", "gp" ],
	               out => [ "flags", "none", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "eflags", "unused", "M" ],
	am        => "source,binary",
	emit      => '. cmp%M %binop',
	attr      => "int ins_permuted, int cmp_unsigned",
	init_attr => "attr->data.ins_permuted   = ins_permuted;\n".
	             "\tattr->data.cmp_unsigned = cmp_unsigned;\n",
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_flags,
	modified_flags => $status_flags
},

Cmp8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx", "eax ebx ecx edx" ] ,
	               out => [ "flags", "none", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "eflags", "unused", "M" ],
	am        => "source,binary",
	emit      => '. cmpb %binop',
	attr      => "int ins_permuted, int cmp_unsigned",
	init_attr => "attr->data.ins_permuted   = ins_permuted;\n".
	             "\tattr->data.cmp_unsigned = cmp_unsigned;\n",
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_flags,
	modified_flags => $status_flags
},

Test => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp" ] ,
	               out => [ "flags", "none", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "eflags", "unused", "M" ],
	am        => "source,binary",
	emit      => '. test%M %binop',
	attr      => "int ins_permuted, int cmp_unsigned",
	init_attr => "attr->data.ins_permuted = ins_permuted;\n".
	             "\tattr->data.cmp_unsigned = cmp_unsigned;\n",
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_flags,
	modified_flags => $status_flags
},

Test8Bit => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx", "eax ebx ecx edx" ] ,
	               out => [ "flags", "none", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "eflags", "unused", "M" ],
	am        => "source,binary",
	emit      => '. testb %binop',
	attr      => "int ins_permuted, int cmp_unsigned",
	init_attr => "attr->data.ins_permuted = ins_permuted;\n".
	             "\tattr->data.cmp_unsigned = cmp_unsigned;\n",
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_flags,
	modified_flags => $status_flags
},

Set => {
	#irn_flags => "R",
	reg_req   => { in => [ "eflags" ], out => [ "eax ebx ecx edx" ] },
	ins       => [ "eflags" ],
	attr_type => "ia32_condcode_attr_t",
	attr      => "pn_Cmp pnc, int ins_permuted",
	init_attr => "attr->attr.data.ins_permuted = ins_permuted;\n".
	              "\tset_ia32_ls_mode(res, mode_Bu);\n",
	emit      => '. set%CMP0 %DB0',
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_gp,
},

SetMem => {
	#irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eflags" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem","eflags" ],
	attr_type => "ia32_condcode_attr_t",
	attr      => "pn_Cmp pnc, int ins_permuted",
	init_attr => "attr->attr.data.ins_permuted = ins_permuted;\n".
	              "\tset_ia32_ls_mode(res, mode_Bu);\n",
	emit      => '. set%CMP3 %AM',
	latency   => 1,
	units     => [ "GP" ],
	mode      => 'mode_M',
},

CMov => {
	#irn_flags => "R",
	# (note: leave the false,true order intact to make it compatible with other
	#  ia32_binary ops)
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "gp", "eflags" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "val_false", "val_true", "eflags" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	attr_type => "ia32_condcode_attr_t",
	attr      => "int ins_permuted, pn_Cmp pnc",
	init_attr => "attr->attr.data.ins_permuted = ins_permuted;",
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_gp,
},

Jcc => {
	state     => "pinned",
	op_flags  => "L|X|Y",
	reg_req   => { in  => [ "eflags" ], out => [ "none", "none" ] },
	ins       => [ "eflags" ],
	outs      => [ "false", "true" ],
	attr_type => "ia32_condcode_attr_t",
	attr      => "pn_Cmp pnc",
	latency   => 2,
	units     => [ "BRANCH" ],
},

SwitchJmp => {
	state     => "pinned",
	op_flags  => "L|X|Y",
	reg_req   => { in => [ "gp" ] },
	mode      => "mode_T",
	attr_type => "ia32_condcode_attr_t",
	attr      => "long pnc",
	latency   => 3,
	units     => [ "BRANCH" ],
	modified_flags => $status_flags,
	init_attr => "info->out_infos = NULL;", # XXX ugly hack for out requirements
},

Jmp => {
	state    => "pinned",
	op_flags => "X",
	reg_req  => { out => [ "none" ] },
	latency  => 1,
	units    => [ "BRANCH" ],
	mode     => "mode_X",
},

IJmp => {
	state     => "pinned",
	op_flags  => "X",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ] },
	ins       => [ "base", "index", "mem", "target" ],
	am        => "source,unary",
	emit      => '. jmp *%unop3',
	latency   => 1,
	units     => [ "BRANCH" ],
	mode      => "mode_X",
	init_attr => "info->out_infos = NULL;", # XXX ugly hack for out requirements
},

Const => {
	op_flags  => "c",
	irn_flags => "R",
	reg_req   => { out => [ "gp" ] },
	units     => [ "GP" ],
	attr      => "ir_entity *symconst, int symconst_sign, int no_pic_adjust, long offset",
	attr_type => "ia32_immediate_attr_t",
	latency   => 1,
	mode      => $mode_gp,
},

GetEIP => {
	op_flags => "c",
	reg_req  => { out => [ "gp" ] },
	units    => [ "GP" ],
	latency  => 5,
	mode     => $mode_gp,
	modified_flags => $status_flags,
},

Unknown_GP => {
	state     => "pinned",
	op_flags  => "c|NB",
	reg_req   => { out => [ "gp_UKNWN:I" ] },
	units     => [],
	emit      => "",
	latency   => 0,
	mode      => $mode_gp
},

Unknown_VFP => {
	state     => "pinned",
	op_flags  => "c|NB",
	reg_req   => { out => [ "vfp_UKNWN:I" ] },
	units     => [],
	emit      => "",
	mode      => "mode_E",
	latency   => 0,
	attr_type => "ia32_x87_attr_t",
},

Unknown_XMM => {
	state     => "pinned",
	op_flags  => "c|NB",
	reg_req   => { out => [ "xmm_UKNWN:I" ] },
	units     => [],
	emit      => "",
	latency   => 0,
	mode      => $mode_xmm
},

NoReg_GP => {
	state     => "pinned",
	op_flags  => "c|NB|NI",
	reg_req   => { out => [ "gp_NOREG:I" ] },
	units     => [],
	emit      => "",
	latency   => 0,
	mode      => $mode_gp
},

NoReg_VFP => {
	state     => "pinned",
	op_flags  => "c|NB|NI",
	reg_req   => { out => [ "vfp_NOREG:I" ] },
	units     => [],
	emit      => "",
	mode      => "mode_E",
	latency   => 0,
	attr_type => "ia32_x87_attr_t",
},

NoReg_XMM => {
	state     => "pinned",
	op_flags  => "c|NB|NI",
	reg_req   => { out => [ "xmm_NOREG:I" ] },
	units     => [],
	emit      => "",
	latency   => 0,
	mode      => "mode_E"
},

ChangeCW => {
	state     => "pinned",
	op_flags  => "c",
	reg_req   => { out => [ "fpcw:I" ] },
	mode      => $mode_fpcw,
	latency   => 3,
	units     => [ "GP" ],
	modified_flags => $fpcw_flags
},

FldCW => {
	op_flags  => "L|F",
	state     => "pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "fpcw:I" ] },
	ins       => [ "base", "index", "mem" ],
	latency   => 5,
	emit      => ". fldcw %AM",
	mode      => $mode_fpcw,
	units     => [ "GP" ],
	modified_flags => $fpcw_flags
},

FnstCW => {
	op_flags  => "L|F",
	state     => "pinned",
	reg_req   => { in => [ "gp", "gp", "none", "fp_cw" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "fpcw" ],
	latency   => 5,
	emit      => ". fnstcw %AM",
	mode      => "mode_M",
	units     => [ "GP" ],
},

FnstCWNOP => {
	op_flags  => "L|F",
	state     => "pinned",
	reg_req   => { in => [ "fp_cw" ], out => [ "none" ] },
	ins       => [ "fpcw" ],
	latency   => 0,
	emit      => "",
	mode      => "mode_M",
},

Cltd => {
	# we should not rematrialize this node. It has very strict constraints.
	reg_req   => { in => [ "eax", "edx" ], out => [ "edx" ] },
	ins       => [ "val", "clobbered" ],
	emit      => '. cltd',
	latency   => 1,
	mode      => $mode_gp,
	units     => [ "GP" ],
},

# Load / Store
#
# Note that we add additional latency values depending on address mode, so a
# lateny of 0 for load is correct

Load => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ],
	               out => [ "gp", "none", "none", "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "res", "unused", "M", "X_exc" ],
	latency   => 0,
	emit      => ". mov%EX%.l %AM, %D0",
	units     => [ "GP" ],
},

Store => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "none", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "M", "X_exc" ],
	emit      => '. mov%M %SI3, %AM',
	latency   => 2,
	units     => [ "GP" ],
	mode      => "mode_M",
},

Store8Bit => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx" ], out => ["none", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "M", "X_exc" ],
	emit      => '. mov%M %SB3, %AM',
	latency   => 2,
	units     => [ "GP" ],
	mode      => "mode_M",
},

Lea => {
	irn_flags => "R",
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp" ] },
	ins       => [ "base", "index" ],
	emit      => '. leal %AM, %D0',
	latency   => 2,
	units     => [ "GP" ],
	mode      => $mode_gp,
# lea doesn't modify the flags, but setting this seems advantageous since it
# increases chances that the Lea is transformed back to an Add
	modified_flags => 1,
},

Push => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp", "esp" ], out => [ "esp:I|S", "none" ] },
	ins       => [ "base", "index", "mem", "val", "stack" ],
	emit      => '. push%M %unop3',
	outs      => [ "stack", "M" ],
	am        => "source,unary",
	latency   => 2,
	units     => [ "GP" ],
},

Pop => {
	state     => "exc_pinned",
	reg_req   => { in => [ "none", "esp" ], out => [ "gp", "none", "none", "esp:I|S" ] },
	ins       => [ "mem", "stack" ],
	outs      => [ "res", "M", "unused", "stack" ],
	emit      => '. pop%M %D0',
	latency   => 3, # Pop is more expensive than Push on Athlon
	units     => [ "GP" ],
},

PopEbp => {
	state     => "exc_pinned",
	reg_req   => { in => [ "none", "esp" ], out => [ "ebp:I", "none", "none", "esp:I|S" ] },
	ins       => [ "mem", "stack" ],
	outs      => [ "res", "M", "unused", "stack" ],
	emit      => '. pop%M %D0',
	latency   => 3, # Pop is more expensive than Push on Athlon
	units     => [ "GP" ],
},

PopMem => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "esp" ], out => [ "none", "none", "none", "esp:I|S" ] },
	ins       => [ "base", "index", "mem", "stack" ],
	outs      => [ "unused0", "M", "unused1", "stack" ],
	emit      => '. pop%M %AM',
	latency   => 3, # Pop is more expensive than Push on Athlon
	units     => [ "GP" ],
},

Enter => {
	reg_req   => { in => [ "esp" ], out => [ "ebp", "esp:I|S", "none" ] },
	emit      => '. enter',
	outs      => [ "frame", "stack", "M" ],
	latency   => 15,
	units     => [ "GP" ],
},

Leave => {
	reg_req   => { in => [ "ebp" ], out => [ "ebp:I", "esp:I|S" ] },
	emit      => '. leave',
	outs      => [ "frame", "stack" ],
	latency   => 3,
	units     => [ "GP" ],
},

AddSP => {
	state     => "pinned",
	reg_req   => { in => [ "gp", "gp", "none", "esp", "gp" ], out => [ "esp:I|S", "none" ] },
	ins       => [ "base", "index", "mem", "stack", "size" ],
	am        => "source,binary",
	emit      => '. addl %binop',
	latency   => 1,
	outs      => [ "stack", "M" ],
	units     => [ "GP" ],
	modified_flags => $status_flags
},

SubSP => {
	state     => "pinned",
	reg_req   => { in => [ "gp", "gp", "none", "esp", "gp" ], out => [ "esp:I|S", "gp", "none" ] },
	ins       => [ "base", "index", "mem", "stack", "size" ],
	am        => "source,binary",
	emit      => ". subl %binop\n".
	             ". movl %%esp, %D1",
	latency   => 2,
	outs      => [ "stack", "addr", "M" ],
	units     => [ "GP" ],
	modified_flags => $status_flags
},

RepPrefix => {
	op_flags  => "K",
	state     => "pinned",
	mode      => "mode_M",
	emit      => ".	rep",
	latency   => 0,
},

LdTls => {
	irn_flags => "R",
	reg_req   => { out => [ "gp" ] },
	units     => [ "GP" ],
	latency   => 1,
},

#
# BT supports source address mode, but this is unused yet
#
Bt => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp" ], out => [ "flags" ] },
	ins       => [ "left", "right" ],
	emit      => '. bt%M %S1, %S0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_flags,
	modified_flags => $status_flags  # only CF is set, but the other flags are undefined
},

Bsf => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ],
	               out => [ "gp", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "operand" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. bsf%M %unop3, %D0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

Bsr => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ],
	               out => [ "gp", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "operand" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. bsr%M %unop3, %D0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

#
# SSE4.2 or SSE4a popcnt instruction
#
Popcnt => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ],
	               out => [ "gp", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "operand" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. popcnt%M %unop3, %D0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
	modified_flags => $status_flags
},

Call => {
	state     => "exc_pinned",
	reg_req   => {
		in  => [ "gp", "gp", "none", "gp", "esp", "fpcw", "eax", "ecx", "edx" ],
		out => [ "esp:I|S", "fpcw:I", "none", "eax", "ecx", "edx", "vf0", "vf1", "vf2", "vf3", "vf4", "vf5", "vf6", "vf7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7" ]
	},
	ins       => [ "base", "index", "mem", "addr", "stack", "fpcw", "eax", "ecx", "edx" ],
	outs      => [ "stack", "fpcw", "M", "eax", "ecx", "edx", "vf0", "vf1", "vf2", "vf3", "vf4", "vf5", "vf6", "vf7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7" ],
	attr_type => "ia32_call_attr_t",
	attr      => "unsigned pop, ir_type *call_tp",
	am        => "source,unary",
	units     => [ "BRANCH" ],
	latency   => 4, # random number
	modified_flags => $status_flags
},

#
# a Helper node for frame-climbing, needed for __builtin_(frame|return)_address
#
# PS: try gcc __builtin_frame_address(100000) :-)
#
ClimbFrame => {
	reg_req   => { in => [ "gp", "gp", "gp"], out => [ "in_r3" ] },
	ins       => [ "frame", "cnt", "tmp" ],
	outs      => [ "res" ],
	latency   => 4, # random number
	attr_type => "ia32_climbframe_attr_t",
	attr      => "unsigned count",
	units     => [ "GP" ],
	mode      => $mode_gp
},

#
# bswap
#
Bswap => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ],
	               out => [ "in_r1" ] },
	emit      => '. bswap%M %S0',
	ins       => [ "val" ],
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
},

#
# bswap16, use xchg here
#
Bswap16 => {
	irn_flags => "R",
	reg_req   => { in => [ "eax ebx ecx edx" ],
	               out => [ "in_r1" ] },
	emit      => '. xchg %SB0, %SH0',
	ins       => [ "val" ],
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
},

#
# BreakPoint
#
Breakpoint => {
	state     => "pinned",
	reg_req   => { in => [ "none" ], out => [ "none" ] },
	ins       => [ "mem" ],
	latency   => 0,
	emit      => ". int3",
	units     => [ "GP" ],
	mode      => mode_M,
},

#
# Undefined Instruction on ALL x86 CPU's
#
UD2 => {
	state     => "pinned",
	reg_req   => { in => [ "none" ], out => [ "none" ] },
	ins       => [ "mem" ],
	latency   => 0,
	emit      => ". .value  0x0b0f",
	units     => [ "GP" ],
	mode      => mode_M,
},

#
# outport
#
Outport => {
	irn_flags => "R",
	state     => "pinned",
	reg_req   => { in => [ "edx", "eax", "none" ], out => [ "none" ] },
	ins       => [ "port", "value", "mem" ],
	emit      => '. out%M %SS0, %SI1',
	units     => [ "GP" ],
	latency   => 1,
	mode      => mode_M,
	modified_flags => $status_flags
},

#
# inport
#
Inport => {
	irn_flags => "R",
	state     => "pinned",
	reg_req   => { in => [ "edx", "none" ], out => [ "eax", "none" ] },
	ins       => [ "port", "mem" ],
	outs      => [ "res", "M" ],
	emit      => '. in%M %DS0, %SS0',
	units     => [ "GP" ],
	latency   => 1,
	mode      => mode_T,
	modified_flags => $status_flags
},

#
# Intel style prefetching
#
Prefetch0 => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "M" ],
	latency   => 0,
	emit      => ". prefetcht0 %AM",
	units     => [ "GP" ],
},

Prefetch1 => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "M" ],
	latency   => 0,
	emit      => ". prefetcht1 %AM",
	units     => [ "GP" ],
},

Prefetch2 => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "M" ],
	latency   => 0,
	emit      => ". prefetcht2 %AM",
	units     => [ "GP" ],
},

PrefetchNTA => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "M" ],
	latency   => 0,
	emit      => ". prefetchnta %AM",
	units     => [ "GP" ],
},

#
# 3DNow! prefetch instructions
#
Prefetch => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "M" ],
	latency   => 0,
	emit      => ". prefetch %AM",
	units     => [ "GP" ],
},

PrefetchW => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "M" ],
	latency   => 0,
	emit      => ". prefetchw %AM",
	units     => [ "GP" ],
},

#-----------------------------------------------------------------------------#
#   _____ _____ ______    __ _             _                     _            #
#  / ____/ ____|  ____|  / _| |           | |                   | |           #
# | (___| (___ | |__    | |_| | ___   __ _| |_   _ __   ___   __| | ___  ___  #
#  \___ \\___ \|  __|   |  _| |/ _ \ / _` | __| | '_ \ / _ \ / _` |/ _ \/ __| #
#  ____) |___) | |____  | | | | (_) | (_| | |_  | | | | (_) | (_| |  __/\__ \ #
# |_____/_____/|______| |_| |_|\___/ \__,_|\__| |_| |_|\___/ \__,_|\___||___/ #
#-----------------------------------------------------------------------------#

# produces a 0/+0.0
xZero => {
	irn_flags => "R",
	reg_req   => { out => [ "xmm" ] },
	emit      => '. xorp%XSD %D0, %D0',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xPzero => {
	irn_flags => "R",
	reg_req   => { out => [ "xmm" ] },
	emit      => '. pxor %D0, %D0',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# produces all 1 bits
xAllOnes => {
	irn_flags => "R",
	reg_req   => { out => [ "xmm" ] },
	emit      => '. pcmpeqb %D0, %D0',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# integer shift left, dword
xPslld => {
	irn_flags => "R",
	reg_req   => { in => [ "xmm", "xmm" ], out => [ "in_r1 !in_r2" ] },
	emit      => '. pslld %SI1, %D0',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# integer shift left, qword
xPsllq => {
	irn_flags => "R",
	reg_req   => { in => [ "xmm", "xmm" ], out => [ "in_r1 !in_r2" ] },
	emit      => '. psllq %SI1, %D0',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# integer shift right, dword
xPsrld => {
	irn_flags => "R",
	reg_req   => { in => [ "xmm", "xmm" ], out => [ "in_r1 !in_r2" ] },
	emit      => '. psrld %SI1, %D0',
	latency   => 1,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# mov from integer to SSE register
xMovd  => {
	irn_flags => "R",
	reg_req   => { in => [ "gp" ], out => [ "xmm" ] },
	emit      => '. movd %S0, %D0',
	latency   => 1,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# commutative operations

xAdd => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. add%XXM %binop',
	latency   => 4,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xMul => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. mul%XXM %binop',
	latency   => 4,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xMax => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. max%XXM %binop',
	latency   => 2,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xMin => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. min%XXM %binop',
	latency   => 2,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xAnd => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. andp%XSD %binop',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xOr => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. orp%XSD %binop',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xXor => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. xorp%XSD %binop',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

# not commutative operations

xAndNot => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 !in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. andnp%XSD %binop',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xSub => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "minuend", "subtrahend" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. sub%XXM %binop',
	latency   => 4,
	units     => [ "SSE" ],
	mode      => $mode_xmm
},

xDiv => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "in_r4 !in_r5", "flags", "none" ] },
	ins       => [ "base", "index", "mem", "dividend", "divisor" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,binary",
	emit      => '. div%XXM %binop',
	latency   => 16,
	units     => [ "SSE" ],
},

# other operations

Ucomi => {
	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm", "xmm" ],
	               out => [ "eflags" ] },
	ins       => [ "base", "index", "mem", "left", "right" ],
	outs      => [ "flags" ],
	am        => "source,binary",
	attr      => "int ins_permuted",
	init_attr => "attr->data.ins_permuted = ins_permuted;",
	emit      => ' .ucomi%XXM %binop',
	latency   => 3,
	units     => [ "SSE" ],
	mode      => $mode_flags,
	modified_flags => 1,
},

# Load / Store

xLoad => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ],
	               out => [ "xmm", "none", "none", "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "res", "unused", "M", "X_exc" ],
	emit      => '. mov%XXM %AM, %D0',
	attr      => "ir_mode *load_mode",
	init_attr => "attr->ls_mode = load_mode;",
	latency   => 0,
	units     => [ "SSE" ],
},

xStore => {
	op_flags => "L|F",
	state    => "exc_pinned",
	reg_req  => { in => [ "gp", "gp", "none", "xmm" ], out => [ "none", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "M", "X_exc" ],
	emit     => '. mov%XXM %S3, %AM',
	latency  => 0,
	units    => [ "SSE" ],
	mode     => "mode_M",
},

xStoreSimple => {
	op_flags => "L|F",
	state    => "exc_pinned",
	reg_req  => { in => [ "gp", "gp", "none", "xmm" ], out => [ "none" ] },
	ins      => [ "base", "index", "mem", "val" ],
	outs     => [ "M" ],
	emit     => '. mov%XXM %S3, %AM',
	latency  => 0,
	units    => [ "SSE" ],
	mode     => "mode_M",
},

CvtSI2SS => {
	op_flags => "L|F",
	state     => "exc_pinned",
	reg_req  => { in => [ "gp", "gp", "none", "gp" ], out => [ "xmm" ] },
	ins      => [ "base", "index", "mem", "val" ],
	am       => "source,unary",
	emit     => '. cvtsi2ss %unop3, %D0',
	latency  => 2,
	units    => [ "SSE" ],
	mode     => $mode_xmm
},

CvtSI2SD => {
	op_flags => "L|F",
	state     => "exc_pinned",
	reg_req  => { in => [ "gp", "gp", "none", "gp" ], out => [ "xmm" ] },
	ins      => [ "base", "index", "mem", "val" ],
	am       => "source,unary",
	emit     => '. cvtsi2sd %unop3, %D0',
	latency  => 2,
	units    => [ "SSE" ],
	mode     => $mode_xmm
},


l_LLtoFloat => {
	op_flags => "L|F",
	cmp_attr => "return 1;",
	ins      => [ "val_high", "val_low" ],
	reg_req  => { in => [ "none", "none" ], out => [ "none" ] }
},

l_FloattoLL => {
	op_flags => "L|F",
	cmp_attr => "return 1;",
	ins      => [ "val" ],
	outs     => [ "res_high", "res_low" ],
	reg_req  => { in => [ "none" ], out => [ "none", "none" ] }
},

# CopyB

CopyB => {
	op_flags  => "F|H",
	state     => "pinned",
	reg_req   => { in => [ "edi", "esi", "ecx", "none" ], out => [ "edi", "esi", "ecx", "none" ] },
	outs      => [ "DST", "SRC", "CNT", "M" ],
	attr_type => "ia32_copyb_attr_t",
	attr      => "unsigned size",
	units     => [ "GP" ],
	latency  => 3,
# we don't care about this flag, so no need to mark this node
#	modified_flags => [ "DF" ]
},

CopyB_i => {
	op_flags  => "F|H",
	state     => "pinned",
	reg_req   => { in => [ "edi", "esi", "none" ], out => [  "edi", "esi", "none" ] },
	outs      => [ "DST", "SRC", "M" ],
	attr_type => "ia32_copyb_attr_t",
	attr      => "unsigned size",
	units     => [ "GP" ],
	latency  => 3,
# we don't care about this flag, so no need to mark this node
#	modified_flags => [ "DF" ]
},

# Conversions

Cwtl => {
	state     => "exc_pinned",
	reg_req   => { in => [ "eax" ], out => [ "eax" ] },
	ins       => [ "val" ],
	outs      => [ "res" ],
	emit      => '. cwtl',
	units     => [ "GP" ],
	latency   => 1,
	mode      => $mode_gp,
},

Conv_I2I => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ],
	               out => [ "gp", "none", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,unary",
	units     => [ "GP" ],
	latency   => 1,
	attr      => "ir_mode *smaller_mode",
	init_attr => "attr->ls_mode = smaller_mode;",
	mode      => $mode_gp,
},

Conv_I2I8Bit => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "eax ebx ecx edx" ],
	               out => [ "gp", "none", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "res", "flags", "M" ],
	am        => "source,unary",
	units     => [ "GP" ],
	latency   => 1,
	attr      => "ir_mode *smaller_mode",
	init_attr => "attr->ls_mode = smaller_mode;",
	mode      => $mode_gp,
},

Conv_I2FP => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "gp" ], out => [ "xmm", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	am        => "source,unary",
	latency   => 10,
	units     => [ "SSE" ],
	mode      => $mode_xmm,
},

Conv_FP2I => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm" ], out => [ "gp", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	am        => "source,unary",
	latency   => 10,
	units     => [ "SSE" ],
	mode      => $mode_gp,
},

Conv_FP2FP => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "xmm" ], out => [ "xmm", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	am        => "source,unary",
	latency   => 8,
	units     => [ "SSE" ],
	mode      => $mode_xmm,
},

#----------------------------------------------------------#
#        _      _               _    __ _             _    #
#       (_)    | |             | |  / _| |           | |   #
# __   ___ _ __| |_ _   _  __ _| | | |_| | ___   __ _| |_  #
# \ \ / / | '__| __| | | |/ _` | | |  _| |/ _ \ / _` | __| #
#  \ V /| | |  | |_| |_| | (_| | | | | | | (_) | (_| | |_  #
#   \_/ |_|_|   \__|\__,_|\__,_|_| |_| |_|\___/ \__,_|\__| #
#                 | |                                      #
#  _ __   ___   __| | ___  ___                             #
# | '_ \ / _ \ / _` |/ _ \/ __|                            #
# | | | | (_) | (_| |  __/\__ \                            #
# |_| |_|\___/ \__,_|\___||___/                            #
#----------------------------------------------------------#

# rematerialisation disabled for all float nodes for now, because the fpcw
# handler runs before spilling and we might end up with wrong fpcw then

vfadd => {
#	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp", "vfp", "fpcw" ], out => [ "vfp" ] },
	ins       => [ "base", "index", "mem", "left", "right", "fpcw" ],
	am        => "source,binary",
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfmul => {
#	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp", "vfp", "fpcw" ], out => [ "vfp" ] },
	ins       => [ "base", "index", "mem", "left", "right", "fpcw" ],
	am        => "source,binary",
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfsub => {
#	irn_flags => "R",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp", "vfp", "fpcw" ], out => [ "vfp" ] },
	ins       => [ "base", "index", "mem", "minuend", "subtrahend", "fpcw" ],
	am        => "source,binary",
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfdiv => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp", "vfp", "fpcw" ], out => [ "vfp", "none" ] },
	ins       => [ "base", "index", "mem", "dividend", "divisor", "fpcw" ],
	am        => "source,binary",
	outs      => [ "res", "M" ],
	latency   => 20,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
},

vfprem => {
	reg_req   => { in => [ "vfp", "vfp", "fpcw" ], out => [ "vfp" ] },
	ins       => [ "left", "right", "fpcw" ],
	latency   => 20,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfabs => {
	irn_flags => "R",
	reg_req   => { in => [ "vfp"], out => [ "vfp" ] },
	ins       => [ "value" ],
	latency   => 2,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfchs => {
	irn_flags => "R",
	reg_req   => { in => [ "vfp"], out => [ "vfp" ] },
	ins       => [ "value" ],
	latency   => 2,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

# virtual Load and Store

vfld => {
	irn_flags => "R",
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ],
	               out => [ "vfp", "none", "none", "none" ] },
	ins       => [ "base", "index", "mem" ],
	outs      => [ "res", "unused", "M", "X_exc" ],
	attr      => "ir_mode *load_mode",
	init_attr => "attr->attr.ls_mode = load_mode;",
	latency   => 2,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
},

vfst => {
	irn_flags => "R",
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp" ],
	               out => [ "none", "none" ] },
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "M", "X_exc" ],
	attr      => "ir_mode *store_mode",
	init_attr => "attr->attr.ls_mode = store_mode;",
	latency   => 2,
	units     => [ "VFP" ],
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
},

# Conversions

vfild => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ],
	               out => [ "vfp", "none", "none" ] },
	outs      => [ "res", "unused", "M" ],
	ins       => [ "base", "index", "mem" ],
	latency   => 4,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
},

vfist => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp", "fpcw" ], out => [ "none" ] },
	ins       => [ "base", "index", "mem", "val", "fpcw" ],
	outs      => [ "M" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
},

# SSE3 fisttp instruction
vfisttp => {
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none", "vfp" ], out => [ "in_r4", "none" ]},
	ins       => [ "base", "index", "mem", "val" ],
	outs      => [ "res", "M" ],
	latency   => 4,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
},


# constants

vfldz => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfld1 => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfldpi => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfldln2 => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfldlg2 => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfldl2t => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

vfldl2e => {
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	outs      => [ "res" ],
	latency   => 4,
	units     => [ "VFP" ],
	mode      => "mode_E",
	attr_type => "ia32_x87_attr_t",
},

# other

vFucomFnstsw => {
# we can't allow to rematerialize this node so we don't have
#  accidently produce Phi(Fucom, Fucom(ins_permuted))
#	irn_flags => "R",
	reg_req   => { in => [ "vfp", "vfp" ], out => [ "eax" ] },
	ins       => [ "left", "right" ],
	outs      => [ "flags" ],
	attr      => "int ins_permuted",
	init_attr => "attr->attr.data.ins_permuted = ins_permuted;",
	latency   => 3,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
	mode      => $mode_gp
},

vFucomi => {
	irn_flags => "R",
	reg_req   => { in => [ "vfp", "vfp" ], out => [ "eflags" ] },
	ins       => [ "left", "right" ],
	outs      => [ "flags" ],
	attr      => "int ins_permuted",
	init_attr => "attr->attr.data.ins_permuted = ins_permuted;",
	latency   => 3,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
	mode      => $mode_gp
},

vFtstFnstsw => {
#	irn_flags => "R",
	reg_req   => { in => [ "vfp" ], out => [ "eax" ] },
	ins       => [ "left" ],
	outs      => [ "flags" ],
	attr      => "int ins_permuted",
	init_attr => "attr->attr.data.ins_permuted = ins_permuted;",
	latency   => 3,
	units     => [ "VFP" ],
	attr_type => "ia32_x87_attr_t",
	mode      => $mode_gp
},

Sahf => {
	irn_flags => "R",
	reg_req   => { in => [ "eax" ], out => [ "eflags" ] },
	ins       => [ "val" ],
	outs      => [ "flags" ],
	emit      => '. sahf',
	latency   => 1,
	units     => [ "GP" ],
	mode      => $mode_flags,
},

#------------------------------------------------------------------------#
#       ___ _____    __ _             _                     _            #
# __  _( _ )___  |  / _| | ___   __ _| |_   _ __   ___   __| | ___  ___  #
# \ \/ / _ \  / /  | |_| |/ _ \ / _` | __| | '_ \ / _ \ / _` |/ _ \/ __| #
#  >  < (_) |/ /   |  _| | (_) | (_| | |_  | | | | (_) | (_| |  __/\__ \ #
# /_/\_\___//_/    |_| |_|\___/ \__,_|\__| |_| |_|\___/ \__,_|\___||___/ #
#------------------------------------------------------------------------#

# Note: gas is strangely buggy: fdivrp and fdivp as well as fsubrp and fsubp
#       are swapped, we work this around in the emitter...

fadd => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fadd%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

faddp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. faddp%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fmul => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fmul%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fmulp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fmulp%XM %x87_binop',,
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fsub => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fsub%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fsubp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
# see note about gas bugs
	emit      => '. fsubrp%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fsubr => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	irn_flags => "R",
	reg_req   => { },
	emit      => '. fsubr%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fsubrp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	irn_flags => "R",
	reg_req   => { },
# see note about gas bugs
	emit      => '. fsubp%XM %x87_binop',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fprem => {
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fprem1',
	latency   => 20,
	attr_type => "ia32_x87_attr_t",
},

# this node is just here, to keep the simulator running
# we can omit this when a fprem simulation function exists
fpremp => {
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fprem1\n'.
	             '. fstp %X0',
	latency   => 20,
	attr_type => "ia32_x87_attr_t",
},

fdiv => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fdiv%XM %x87_binop',
	latency   => 20,
	attr_type => "ia32_x87_attr_t",
},

fdivp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
# see note about gas bugs
	emit      => '. fdivrp%XM %x87_binop',
	latency   => 20,
	attr_type => "ia32_x87_attr_t",
},

fdivr => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fdivr%XM %x87_binop',
	latency   => 20,
	attr_type => "ia32_x87_attr_t",
},

fdivrp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
# see note about gas bugs
	emit      => '. fdivp%XM %x87_binop',
	latency   => 20,
	attr_type => "ia32_x87_attr_t",
},

fabs => {
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fabs',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

fchs => {
	op_flags  => "R|K",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fchs',
	latency   => 4,
	attr_type => "ia32_x87_attr_t",
},

# x87 Load and Store

fld => {
	rd_constructor => "NONE",
	op_flags  => "R|L|F",
	state     => "exc_pinned",
	reg_req   => { },
	emit      => '. fld%XM %AM',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fst => {
	rd_constructor => "NONE",
	op_flags  => "R|L|F",
	state     => "exc_pinned",
	reg_req   => { },
	emit      => '. fst%XM %AM',
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fstp => {
	rd_constructor => "NONE",
	op_flags  => "R|L|F",
	state     => "exc_pinned",
	reg_req   => { },
	emit      => '. fstp%XM %AM',
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

# Conversions

fild => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fild%XM %AM',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fist => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fist%XM %AM',
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fistp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fistp%XM %AM',
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

# SSE3 fisttp instruction
fisttp => {
	state     => "exc_pinned",
	rd_constructor => "NONE",
	reg_req   => { },
	emit      => '. fisttp%XM %AM',
	mode      => "mode_M",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

# constants

fldz => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fldz',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fld1 => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fld1',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fldpi => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fldpi',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fldln2 => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fldln2',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fldlg2 => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fldlg2',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fldl2t => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fldll2t',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

fldl2e => {
	op_flags  => "R|c|K",
	irn_flags => "R",
	reg_req   => { out => [ "vfp" ] },
	emit      => '. fldl2e',
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

# fxch, fpush, fpop
# Note that it is NEVER allowed to do CSE on these nodes
# Moreover, note the virtual register requierements!

fxch => {
	op_flags  => "R|K",
	reg_req   => { out => [ "none" ] },
	cmp_attr  => "return 1;",
	emit      => '. fxch %X0',
	attr_type => "ia32_x87_attr_t",
	mode      => "mode_ANY",
	latency   => 1,
},

fpush => {
	op_flags  => "R|K",
	reg_req   => { out => [ "none" ] },
	cmp_attr  => "return 1;",
	emit      => '. fld %X0',
	attr_type => "ia32_x87_attr_t",
	mode      => "mode_ANY",
	latency   => 1,
},

fpushCopy => {
	reg_req   => { in => [ "vfp"], out => [ "vfp" ] },
	cmp_attr  => "return 1;",
	emit      => '. fld %X0',
	attr_type => "ia32_x87_attr_t",
	latency   => 1,
},

fpop => {
	op_flags  => "K",
	reg_req   => { out => [ "none" ] },
	cmp_attr  => "return 1;",
	emit      => '. fstp %X0',
	attr_type => "ia32_x87_attr_t",
	mode      => "mode_ANY",
	latency   => 1,
},

ffreep => {
	op_flags  => "K",
	reg_req   => { out => [ "none" ] },
	cmp_attr  => "return 1;",
	emit      => '. ffreep %X0',
	attr_type => "ia32_x87_attr_t",
	mode      => "mode_ANY",
	latency   => 1,
},

emms => {
	op_flags  => "K",
	reg_req   => { out => [ "none" ] },
	cmp_attr  => "return 1;",
	emit      => '. emms',
	attr_type => "ia32_x87_attr_t",
	mode      => "mode_ANY",
	latency   => 3,
},

femms => {
	op_flags  => "K",
	reg_req   => { out => [ "none" ] },
	cmp_attr  => "return 1;",
	emit      => '. femms',
	attr_type => "ia32_x87_attr_t",
	mode      => "mode_ANY",
	latency   => 3,
},

# compare

FucomFnstsw => {
	reg_req   => { },
	emit      => ". fucom %X1\n".
	             ". fnstsw %%ax",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

FucompFnstsw => {
	reg_req   => { },
	emit      => ". fucomp %X1\n".
	             ". fnstsw %%ax",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

FucomppFnstsw => {
	reg_req   => { },
	emit      => ". fucompp\n".
	             ". fnstsw %%ax",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},

Fucomi => {
	reg_req   => { },
	emit      => '. fucomi %X1',
	attr_type => "ia32_x87_attr_t",
	latency   => 1,
},

Fucompi => {
	reg_req   => { },
	emit      => '. fucompi %X1',
	attr_type => "ia32_x87_attr_t",
	latency   => 1,
},

FtstFnstsw => {
	reg_req   => { },
	emit      => ". ftst\n".
	             ". fnstsw %%ax",
	attr_type => "ia32_x87_attr_t",
	latency   => 2,
},


# -------------------------------------------------------------------------------- #
#  ____ ____  _____                  _                               _             #
# / ___/ ___|| ____| __   _____  ___| |_ ___  _ __   _ __   ___   __| | ___  ___   #
# \___ \___ \|  _|   \ \ / / _ \/ __| __/ _ \| '__| | '_ \ / _ \ / _` |/ _ \/ __|  #
#  ___) |__) | |___   \ V /  __/ (__| || (_) | |    | | | | (_) | (_| |  __/\__ \  #
# |____/____/|_____|   \_/ \___|\___|\__\___/|_|    |_| |_|\___/ \__,_|\___||___/  #
#                                                                                  #
# -------------------------------------------------------------------------------- #


# Spilling and reloading of SSE registers, hardcoded, not generated #

xxLoad => {
	op_flags  => "L|F",
	state     => "exc_pinned",
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "xmm", "none" ] },
	emit      => '. movdqu %D0, %AM',
	outs      => [ "res", "M" ],
	units     => [ "SSE" ],
	latency   => 1,
},

xxStore => {
	op_flags => "L|F",
	state    => "exc_pinned",
	reg_req  => { in => [ "gp", "gp", "none", "xmm" ] },
	ins      => [ "base", "index", "mem", "val" ],
	emit     => '. movdqu %binop',
	units    => [ "SSE" ],
	latency   => 1,
	mode     => "mode_M",
},

); # end of %nodes

# Include the generated SIMD node specification written by the SIMD optimization
$my_script_name = dirname($myname) . "/../ia32/ia32_simd_spec.pl";
unless ($return = do $my_script_name) {
	warn "couldn't parse $my_script_name: $@" if $@;
	warn "couldn't do $my_script_name: $!"    unless defined $return;
	warn "couldn't run $my_script_name"       unless $return;
}

# Transform some attributes
foreach my $op (keys(%nodes)) {
	my $node         = $nodes{$op};
	my $op_attr_init = $node->{op_attr_init};

	if(defined($op_attr_init)) {
		$op_attr_init .= "\n\t";
	} else {
		$op_attr_init = "";
	}

	if(!defined($node->{latency})) {
		if($op =~ m/^l_/) {
			$node->{latency} = 0;
		} else {
			die("Latency missing for op $op");
		}
	}
	$op_attr_init .= "attr->latency = ".$node->{latency} . ";";

	$node->{op_attr_init} = $op_attr_init;
}

print "";
