# Arm Architecure Specification
# Author: Matthias Braun, Michael Beck, Oliver Richter, Tobias Gneist

$arch = "arm";

#
# Modes
#
$mode_gp    = "mode_Iu";
$mode_flags = "mode_Bu";
$mode_fp    = "mode_F";

# NOTE: Last entry of each class is the largest Firm-Mode a register can hold
%reg_classes = (
	gp => [
		{ name => "r0",  dwarf => 0 },
		{ name => "r1",  dwarf => 1 },
		{ name => "r2",  dwarf => 2 },
		{ name => "r3",  dwarf => 3 },
		{ name => "r4",  dwarf => 4 },
		{ name => "r5",  dwarf => 5 },
		{ name => "r6",  dwarf => 6 },
		{ name => "r7",  dwarf => 7 },
		{ name => "r8",  dwarf => 8 },
		{ name => "r9",  dwarf => 9 },
		{ name => "r10", dwarf => 10 },
		{ name => "r11", dwarf => 11 },
		{ name => "r12", dwarf => 12 },
		{ name => "sp",  dwarf => 13 },
		{ name => "lr",  dwarf => 14 },
		{ name => "pc",  dwarf => 15 },
		{ mode => $mode_gp }
	],
	fpa => [
		{ name => "f0", dwarf => 96 },
		{ name => "f1", dwarf => 97 },
		{ name => "f2", dwarf => 98 },
		{ name => "f3", dwarf => 99 },
		{ name => "f4", dwarf => 100 },
		{ name => "f5", dwarf => 101 },
		{ name => "f6", dwarf => 102 },
		{ name => "f7", dwarf => 103 },
		{ mode => $mode_fp }
	],
	flags => [
		{ name => "fl" },
		{ mode => $mode_flags, flags => "manual_ra" }
	],
);

$default_attr_type = "arm_attr_t";
$default_copy_attr = "arm_copy_attr";

%init_attr = (
	arm_attr_t           => "\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);",
	arm_SymConst_attr_t  =>
		"\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);\n".
		"\tinit_arm_SymConst_attributes(res, entity, symconst_offset);",
	arm_CondJmp_attr_t   => "\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);",
	arm_SwitchJmp_attr_t => "\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);",
	arm_fConst_attr_t    => "\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);",
	arm_load_store_attr_t =>
		"\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);\n".
		"\tinit_arm_load_store_attributes(res, ls_mode, entity, entity_sign, offset, is_frame_entity);",
	arm_shifter_operand_t =>
		"\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);\n",
	arm_cmp_attr_t =>
		"\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);\n",
	arm_farith_attr_t =>
		"\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);\n".
		"\tinit_arm_farith_attributes(res, op_mode);",
	arm_CopyB_attr_t =>
		"\tinit_arm_attributes(res, irn_flags_, in_reqs, n_res);\n".
		"\tinit_arm_CopyB_attributes(res, size);",
);

%compare_attr = (
	arm_attr_t            => "cmp_attr_arm",
	arm_SymConst_attr_t   => "cmp_attr_arm_SymConst",
	arm_CondJmp_attr_t    => "cmp_attr_arm_CondJmp",
	arm_SwitchJmp_attr_t  => "cmp_attr_arm_SwitchJmp",
	arm_fConst_attr_t     => "cmp_attr_arm_fConst",
	arm_load_store_attr_t => "cmp_attr_arm_load_store",
	arm_shifter_operand_t => "cmp_attr_arm_shifter_operand",
	arm_CopyB_attr_t      => "cmp_attr_arm_CopyB",
	arm_cmp_attr_t        => "cmp_attr_arm_cmp",
	arm_farith_attr_t     => "cmp_attr_arm_farith",
);

my %unop_shifter_operand_constructors = (
	imm => {
		attr       => "unsigned char immediate_value, unsigned char immediate_rot",
		custominit => "init_arm_shifter_operand(res, immediate_value, ARM_SHF_IMM, immediate_rot);",
		reg_req    => { in => [], out => [ "gp" ] },
	},
	reg => {
		custominit => "init_arm_shifter_operand(res, 0, ARM_SHF_REG, 0);",
		reg_req    => { in => [ "gp" ], out => [ "gp" ] },
	},
	reg_shift_reg => {
		attr       => "arm_shift_modifier_t shift_modifier",
		custominit => "init_arm_shifter_operand(res, 0, shift_modifier, 0);",
		reg_req    => { in => [ "gp", "gp" ], out => [ "gp" ] },
	},
	reg_shift_imm => {
		attr       => "arm_shift_modifier_t shift_modifier, unsigned shift_immediate",
		custominit => "init_arm_shifter_operand(res, 0, shift_modifier, shift_immediate);",
		reg_req    => { in => [ "gp" ], out => [ "gp" ] },
	},
);

my %binop_shifter_operand_constructors = (
	imm => {
		attr       => "unsigned char immediate_value, unsigned char immediate_rot",
		custominit => "init_arm_shifter_operand(res, immediate_value, ARM_SHF_IMM, immediate_rot);",
		reg_req    => { in => [ "gp" ], out => [ "gp" ] },
		ins        => [ "left" ],
	},
	reg => {
		custominit => "init_arm_shifter_operand(res, 0, ARM_SHF_REG, 0);",
		reg_req    => { in => [ "gp", "gp" ], out => [ "gp" ] },
		ins        => [ "left", "right" ],
	},
	reg_shift_reg => {
		attr       => "arm_shift_modifier_t shift_modifier",
		custominit => "init_arm_shifter_operand(res, 0, shift_modifier, 0);",
		reg_req    => { in => [ "gp", "gp", "gp" ], out => [ "gp" ] },
		ins        => [ "left", "right", "shift" ],
	},
	reg_shift_imm => {
		attr       => "arm_shift_modifier_t shift_modifier, unsigned shift_immediate",
		custominit => "init_arm_shifter_operand(res, 0, shift_modifier, shift_immediate);",
		reg_req    => { in => [ "gp", "gp" ], out => [ "gp" ] },
		ins        => [ "left", "right" ],
	},
);

my %cmp_shifter_operand_constructors = (
	imm => {
		attr       => "unsigned char immediate_value, unsigned char immediate_rot, bool ins_permuted, bool is_unsigned",
		custominit =>
			"init_arm_shifter_operand(res, immediate_value, ARM_SHF_IMM, immediate_rot);\n".
			"\tinit_arm_cmp_attr(res, ins_permuted, is_unsigned);",
		reg_req    => { in => [ "gp" ], out => [ "flags" ] },
		ins        => [ "left" ],
	},
	reg => {
		attr       => "bool ins_permuted, bool is_unsigned",
		custominit =>
			"init_arm_shifter_operand(res, 0, ARM_SHF_REG, 0);\n".
			"\tinit_arm_cmp_attr(res, ins_permuted, is_unsigned);",
		reg_req    => { in => [ "gp", "gp" ], out => [ "flags" ] },
		ins        => [ "left", "right" ],
	},
	reg_shift_reg => {
		attr       => "arm_shift_modifier_t shift_modifier, bool ins_permuted, bool is_unsigned",
		custominit =>
			"init_arm_shifter_operand(res, 0, shift_modifier, 0);\n".
			"\tinit_arm_cmp_attr(res, ins_permuted, is_unsigned);",
		reg_req    => { in => [ "gp", "gp", "gp" ], out => [ "flags" ] },
		ins        => [ "left", "right", "shift" ],
	},
	reg_shift_imm => {
		attr       => "arm_shift_modifier_t shift_modifier, unsigned shift_immediate, bool ins_permuted, bool is_unsigned",
		custominit =>
			"init_arm_shifter_operand(res, 0, shift_modifier, shift_immediate);\n".
			"\tinit_arm_cmp_attr(res, ins_permuted, is_unsigned);",
		reg_req    => { in => [ "gp", "gp" ], out => [ "flags" ] },
		ins        => [ "left", "right" ],
	},
);


%nodes = (

Add => {
	irn_flags => [ "rematerializable" ],
	emit      => 'add %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Mul => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "gp", "gp" ], out => [ "!in_r1" ] },
	emit      => 'mul %D0, %S0, %S1',
	mode      => $mode_gp,
},

Smull => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp", "gp" ] },
	emit      => 'smull %D0, %D1, %S0, %S1',
	outs      => [ "low", "high" ],
},

Umull => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "gp", "gp" ], out => [ "gp", "gp" ] },
	emit      =>'umull %D0, %D1, %S0, %S1',
	outs      => [ "low", "high" ],
	mode      => $mode_gp,
},

Mla => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "gp", "gp", "gp" ], out => [ "!in_r1" ] },
	emit      =>'mla %D0, %S0, %S1, %S2',
	mode      => $mode_gp,
},

And => {
	irn_flags => [ "rematerializable" ],
	emit      => 'and %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Or => {
	irn_flags => [ "rematerializable" ],
	emit      => 'orr %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Eor => {
	irn_flags => [ "rematerializable" ],
	emit      => 'eor %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Bic => {
	irn_flags => [ "rematerializable" ],
	emit      => 'bic %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Sub => {
	irn_flags => [ "rematerializable" ],
	emit      => 'sub %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Rsb => {
	irn_flags => [ "rematerializable" ],
	emit      => 'rsb %D0, %S0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%binop_shifter_operand_constructors,
},

Mov => {
	irn_flags => [ "rematerializable" ],
	emit      => 'mov %D0, %O',
	mode      => $mode_gp,
	attr_type => "arm_shifter_operand_t",
	constructors => \%unop_shifter_operand_constructors,
},

Mvn => {
	irn_flags => [ "rematerializable" ],
	attr_type => "arm_shifter_operand_t",
	emit      => 'mvn %D0, %O',
	mode      => $mode_gp,
	constructors => \%unop_shifter_operand_constructors,
},

Clz => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	emit      => 'clz %D0, %S0',
	mode      => $mode_gp,
},

# mov lr, pc\n mov pc, XXX -- This combination is used for calls to function
# pointers
LinkMovPC => {
	state        => "exc_pinned",
	arity        => "variable",
	out_arity    => "variable",
	attr_type    => "arm_shifter_operand_t",
	attr         => "arm_shift_modifier_t shift_modifier, unsigned char immediate_value, unsigned char immediate_rot",
	custominit   => "init_arm_shifter_operand(res, immediate_value, shift_modifier, immediate_rot);\n".
	                "\tarch_add_irn_flags(res, arch_irn_flags_modify_flags);",
	emit         => "mov lr, pc\n".
	                "mov pc, %O",
},

# mov lr, pc\n ldr pc, XXX -- This combination is used for calls to function
# pointers
LinkLdrPC => {
	state        => "exc_pinned",
	arity        => "variable",
	out_arity    => "variable",
	attr_type    => "arm_load_store_attr_t",
	attr         => "ir_mode *ls_mode, ir_entity *entity, int entity_sign, long offset, bool is_frame_entity",
	custominit   => "arch_add_irn_flags(res, arch_irn_flags_modify_flags);",
	emit         => "mov lr, pc\n".
	                "ldr pc, %O",
},

Bl => {
	state      => "exc_pinned",
	arity      => "variable",
	out_arity  => "variable",
	attr_type  => "arm_SymConst_attr_t",
	attr       => "ir_entity *entity, int symconst_offset",
	custominit => "arch_add_irn_flags(res, arch_irn_flags_modify_flags);",
	emit       => 'bl %I',
},

# this node produces ALWAYS an empty (tempary) gp reg and cannot be CSE'd
EmptyReg => {
	op_flags  => [ "constlike" ],
	irn_flags => [ "rematerializable" ],
	reg_req   => { out => [ "gp" ] },
	emit      => '/* %D0 now available for calculations */',
	cmp_attr  => 'return 1;',
	mode      => $mode_gp,
},

CopyB => {
	state     => "pinned",
	attr      => "unsigned size",
	attr_type => "arm_CopyB_attr_t",
	reg_req   => { in => [ "!sp", "!sp", "gp", "gp", "gp", "none" ], out => [ "none" ] },
	outs      => [ "M" ],
},

FrameAddr => {
	op_flags  => [ "constlike" ],
	irn_flags => [ "rematerializable" ],
	attr      => "ir_entity *entity, int symconst_offset",
	reg_req   => { in => [ "gp" ], out => [ "gp" ] },
	ins       => [ "base" ],
	attr_type => "arm_SymConst_attr_t",
	mode      => $mode_gp,
},

SymConst => {
	op_flags  => [ "constlike" ],
	irn_flags => [ "rematerializable" ],
	attr      => "ir_entity *entity, int symconst_offset",
	reg_req   => { out => [ "gp" ] },
	attr_type => "arm_SymConst_attr_t",
	mode      => $mode_gp,
},

Cmp => {
	irn_flags    => [ "rematerializable", "modify_flags" ],
	emit         => 'cmp %S0, %O',
	mode         => $mode_flags,
	attr_type    => "arm_cmp_attr_t",
	constructors => \%cmp_shifter_operand_constructors,
},

Tst => {
	irn_flags    => [ "rematerializable", "modify_flags" ],
	emit         => 'tst %S0, %O',
	mode         => $mode_flags,
	attr_type    => "arm_cmp_attr_t",
	constructors => \%cmp_shifter_operand_constructors,
},

B => {
	op_flags  => [ "cfopcode", "forking" ],
	state     => "pinned",
	mode      => "mode_T",
	reg_req   => { in => [ "flags" ], out => [ "none", "none" ] },
	attr      => "ir_relation relation",
	attr_type => "arm_CondJmp_attr_t",
	init_attr => "\tset_arm_CondJmp_relation(res, relation);",
},

Jmp => {
	state     => "pinned",
	op_flags  => [ "cfopcode" ],
	irn_flags => [ "simple_jump" ],
	reg_req   => { out => [ "none" ] },
	mode      => "mode_X",
},

SwitchJmp => {
	op_flags  => [ "cfopcode", "forking" ],
	state     => "pinned",
	mode      => "mode_T",
	attr      => "const ir_switch_table *table",
	init_attr => "init_arm_SwitchJmp_attributes(res, table);",
	reg_req   => { in => [ "gp" ], out => [ "none" ] },
	out_arity => "variable",
	attr_type => "arm_SwitchJmp_attr_t",
},

Ldr => {
	op_flags  => [ "uses_memory" ],
	state     => "exc_pinned",
	ins       => [ "ptr", "mem" ],
	outs      => [ "res", "M" ],
	reg_req   => { in => [ "gp", "none" ], out => [ "gp", "none" ] },
	emit      => 'ldr%ML %D0, [%S0, #%o]',
	attr_type => "arm_load_store_attr_t",
	attr      => "ir_mode *ls_mode, ir_entity *entity, int entity_sign, long offset, bool is_frame_entity",
},

Str => {
	op_flags  => [ "uses_memory" ],
	state     => "exc_pinned",
	ins       => [ "ptr", "val", "mem" ],
	outs      => [ "M" ],
	reg_req   => { in => [ "gp", "gp", "none" ], out => [ "none" ] },
	emit      => 'str%MS %S1, [%S0, #%o]',
	mode      => "mode_M",
	attr_type => "arm_load_store_attr_t",
	attr      => "ir_mode *ls_mode, ir_entity *entity, int entity_sign, long offset, bool is_frame_entity",
},

StoreStackM4Inc => {
	op_flags  => [ "uses_memory" ],
	irn_flags => [ "rematerializable" ],
	state     => "exc_pinned",
	reg_req   => { in => [ "sp", "gp", "gp", "gp", "gp", "none" ], out => [ "sp:I|S", "none" ] },
	emit      => 'stmfd %S0!, {%S1, %S2, %S3, %S4}',
	outs      => [ "ptr", "M" ],
},

LoadStackM3Epilogue => {
	op_flags  => [ "uses_memory" ],
	irn_flags => [ "rematerializable" ],
	state     => "exc_pinned",
	reg_req   => { in => [ "sp", "none" ], out => [ "r11:I", "sp:I|S", "pc:I", "none" ] },
	emit      => 'ldmfd %S0, {%D0, %D1, %D2}',
	outs      => [ "res0", "res1", "res2", "M" ],
},



Adf => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "fpa", "fpa" ], out => [ "fpa" ] },
	emit      => 'adf%MA %D0, %S0, %S1',
	attr_type => "arm_farith_attr_t",
	attr      => "ir_mode *op_mode",
	mode      => $mode_fp,
},

Muf => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "fpa", "fpa" ], out => [ "fpa" ] },
	emit      => 'muf%MA %D0, %S0, %S1',
	attr_type => "arm_farith_attr_t",
	attr      => "ir_mode *op_mode",
	mode      => $mode_fp,
},

Suf => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "fpa", "fpa" ], out => [ "fpa" ] },
	emit      => 'suf%MA %D0, %S0, %S1',
	attr_type => "arm_farith_attr_t",
	attr      => "ir_mode *op_mode",
	mode      => $mode_fp,
},

Dvf => {
	reg_req   => { in => [ "fpa", "fpa" ], out => [ "fpa", "none" ] },
	emit      => 'dvf%MA %D0, %S0, %S1',
	outs      => [ "res", "M" ],
	attr_type => "arm_farith_attr_t",
	attr      => "ir_mode *op_mode",
	mode      => $mode_fp,
},

Mvf => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "fpa" ], out => [ "fpa" ] },
	emit      => 'mvf%MA %S0, %D0',
	attr_type => "arm_farith_attr_t",
	attr      => "ir_mode *op_mode",
	mode      => $mode_fp,
},

FltX => {
	irn_flags => [ "rematerializable" ],
	reg_req   => { in => [ "gp" ], out => [ "fpa" ] },
	emit      => 'flt%MA %D0, %S0',
	attr_type => "arm_farith_attr_t",
	attr      => "ir_mode *op_mode",
	mode      => $mode_fp,
},

Cmfe => {
	irn_flags => [ "rematerializable", "modify_flags" ],
	mode      => $mode_flags,
	attr_type => "arm_cmp_attr_t",
	attr      => "bool ins_permuted",
	init_attr => "init_arm_cmp_attr(res, ins_permuted, false);",
	reg_req   => { in => [ "fpa", "fpa" ], out => [ "flags" ] },
	emit      => 'cmfe %S0, %S1',
},

Ldf => {
	op_flags  => [ "uses_memory" ],
	state     => "exc_pinned",
	ins       => [ "ptr", "mem" ],
	outs      => [ "res", "M" ],
	reg_req   => { in => [ "gp", "none" ], out => [ "fpa", "none" ] },
	emit      => 'ldf%MF %D0, [%S0, #%o]',
	attr_type => "arm_load_store_attr_t",
	attr      => "ir_mode *ls_mode, ir_entity *entity, int entity_sign, long offset, bool is_frame_entity",
},

Stf => {
	op_flags  => [ "uses_memory" ],
	state     => "exc_pinned",
	ins       => [ "ptr", "val", "mem" ],
	outs      => [ "M" ],
	mode      => "mode_M",
	reg_req   => { in => [ "gp", "fpa", "none" ], out => [ "none" ] },
	emit      => 'stf%MF %S1, [%S0, #%o]',
	attr_type => "arm_load_store_attr_t",
	attr      => "ir_mode *ls_mode, ir_entity *entity, int entity_sign, long offset, bool is_frame_entity",
},

#
# floating point constants
#
fConst => {
	op_flags  => [ "constlike" ],
	irn_flags => [ "rematerializable" ],
	attr      => "ir_tarval *tv",
	init_attr => "attr->tv = tv;",
	mode      => "get_tarval_mode(tv)",
	reg_req   => { out => [ "fpa" ] },
	attr_type => "arm_fConst_attr_t",
}

); # end of %nodes
