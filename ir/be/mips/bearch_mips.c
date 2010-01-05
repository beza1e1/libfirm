/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   The main mips backend driver file.
 * @author  Matthias Braun, Mehdi
 * @version $Id$
 */
#include "config.h"

#include "pseudo_irg.h"
#include "irgwalk.h"
#include "irprog.h"
#include "irprintf.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgopt.h"
#include "irgwalk.h"
#include "iredges.h"
#include "irdump.h"
#include "irextbb.h"
#include "irtools.h"
#include "error.h"

#include "bitset.h"
#include "debug.h"

#include "../bearch.h"
#include "../benode.h"
#include "../belower.h"
#include "../besched.h"
#include "../beblocksched.h"
#include "../beirg.h"
#include "be.h"
#include "../beabi.h"
#include "../bemachine.h"
#include "../bemodule.h"
#include "../bespillslots.h"
#include "../beemitter.h"
#include "../begnuas.h"

#include "bearch_mips_t.h"

#include "mips_new_nodes.h"
#include "gen_mips_regalloc_if.h"
#include "mips_transform.h"
#include "mips_emitter.h"
#include "mips_map_regs.h"
#include "mips_util.h"
#include "mips_scheduler.h"

#define DEBUG_MODULE "firm.be.mips.isa"

/* TODO: ugly, but we need it to get access to the registers assigned to Phi nodes */
static set *cur_reg_set = NULL;

/**************************************************
 *                         _ _              _  __
 *                        | | |            (_)/ _|
 *  _ __ ___  __ _    __ _| | | ___   ___   _| |_
 * | '__/ _ \/ _` |  / _` | | |/ _ \ / __| | |  _|
 * | | |  __/ (_| | | (_| | | | (_) | (__  | | |
 * |_|  \___|\__, |  \__,_|_|_|\___/ \___| |_|_|
 *            __/ |
 *           |___/
 **************************************************/

static arch_irn_class_t mips_classify(const ir_node *irn)
{
	(void) irn;
	return 0;
}

int mips_is_Load(const ir_node *node)
{
	return is_mips_lw(node) || is_mips_lh(node) || is_mips_lhu(node) ||
		is_mips_lb(node) || is_mips_lbu(node);
}

int mips_is_Store(const ir_node *node)
{
	return is_mips_sw(node) || is_mips_sh(node) || is_mips_sb(node);
}

static ir_entity *mips_get_frame_entity(const ir_node *node)
{
	const mips_load_store_attr_t *attr;

	if(!is_mips_irn(node))
		return NULL;
	if(!mips_is_Load(node) && !mips_is_Store(node))
		return NULL;

	attr = get_mips_load_store_attr_const(node);
	return attr->stack_entity;
}

static void mips_set_frame_entity(ir_node *node, ir_entity *entity)
{
	mips_load_store_attr_t *attr;

	if(!is_mips_irn(node)) {
		panic("trying to set frame entity on non load/store node %+F", node);
	}
	if(!mips_is_Load(node) && !mips_is_Store(node)) {
		panic("trying to set frame entity on non load/store node %+F", node);
	}

	attr = get_irn_generic_attr(node);
	attr->stack_entity = entity;
}

/**
 * This function is called by the generic backend to correct offsets for
 * nodes accessing the stack.
 */
static void mips_set_frame_offset(ir_node *node, int offset)
{
	mips_load_store_attr_t *attr;

	if(!is_mips_irn(node)) {
		panic("trying to set frame offset on non load/store node %+F", node);
	}
	if(!mips_is_Load(node) && !mips_is_Store(node)) {
		panic("trying to set frame offset on non load/store node %+F", node);
	}

	attr = get_irn_generic_attr(node);
	attr->offset += offset;

	if(attr->offset < -32768 || attr->offset > 32767) {
		panic("Out of stack space! (mips supports only 16bit offsets)");
	}
}

static int mips_get_sp_bias(const ir_node *irn)
{
	(void) irn;
	return 0;
}

/* fill register allocator interface */

static const arch_irn_ops_t mips_irn_ops = {
	get_mips_in_req,
	mips_classify,
	mips_get_frame_entity,
	mips_set_frame_entity,
	mips_set_frame_offset,
	mips_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

/**************************************************
 *                _                         _  __
 *               | |                       (_)/ _|
 *   ___ ___   __| | ___  __ _  ___ _ __    _| |_
 *  / __/ _ \ / _` |/ _ \/ _` |/ _ \ '_ \  | |  _|
 * | (_| (_) | (_| |  __/ (_| |  __/ | | | | | |
 *  \___\___/ \__,_|\___|\__, |\___|_| |_| |_|_|
 *                        __/ |
 *                       |___/
 **************************************************/

/**
 * Transforms the standard firm graph into
 * a mips firm graph
 */
static void mips_prepare_graph(void *self) {
	mips_code_gen_t *cg = self;

	/* do local optimizations */
	optimize_graph_df(cg->irg);

	/* TODO: we often have dead code reachable through out-edges here. So for
	 * now we rebuild edges (as we need correct user count for code selection)
	 */
#if 1
	edges_deactivate(cg->irg);
	edges_activate(cg->irg);
#endif

	// walk the graph and transform firm nodes into mips nodes where possible
	mips_transform_graph(cg);
	dump_ir_block_graph_sched(cg->irg, "-transformed");

	/* do local optimizations (mainly CSE) */
	optimize_graph_df(cg->irg);

	/* do code placement, to optimize the position of constants */
	place_code(cg->irg);

	be_dump(cg->irg, "-place", dump_ir_block_graph_sched);
}

/**
 * Called immediately before emit phase.
 */
static void mips_finish_irg(void *self) {
	mips_code_gen_t *cg = self;
	ir_graph        *irg = cg->irg;

	/* create block schedule, this also removes empty blocks which might
	 * produce critical edges */
   	cg->block_schedule = be_create_block_schedule(irg, cg->birg->exec_freq);

	dump_ir_block_graph_sched(irg, "-mips-finished");
}


static void mips_before_ra(void *self)
{
	(void) self;
}

static void mips_after_ra(void* self)
{
	mips_code_gen_t *cg = self;
	be_coalesce_spillslots(cg->birg);
	irg_walk_blkwise_graph(cg->irg, NULL, mips_after_ra_walker, self);
}

/**
 * Emits the code, closes the output file and frees
 * the code generator interface.
 */
static void mips_emit_and_done(void *self)
{
	mips_code_gen_t *cg  = self;
	ir_graph        *irg = cg->irg;
	(void) self;

	mips_gen_routine(cg, irg);

	cur_reg_set = NULL;

	/* de-allocate code generator */
	del_set(cg->reg_set);
	free(cg);
}

static void *mips_cg_init(be_irg_t *birg);

static const arch_code_generator_if_t mips_code_gen_if = {
	mips_cg_init,
	NULL,                /* get_pic_base */
	NULL,                /* before abi introduce */
	mips_prepare_graph,
	NULL,                /* spill */
	mips_before_ra,      /* before register allocation hook */
	mips_after_ra,
	mips_finish_irg,
	mips_emit_and_done
};

/**
 * Initializes the code generator.
 */
static void *mips_cg_init(be_irg_t *birg)
{
	const arch_env_t *arch_env = be_get_birg_arch_env(birg);
	mips_isa_t       *isa      = (mips_isa_t *) arch_env;
	mips_code_gen_t  *cg       = XMALLOCZ(mips_code_gen_t);

	cg->impl     = &mips_code_gen_if;
	cg->irg      = be_get_birg_irg(birg);
	cg->reg_set  = new_set(mips_cmp_irn_reg_assoc, 1024);
	cg->isa      = isa;
	cg->birg     = birg;

	cur_reg_set = cg->reg_set;

	isa->cg = cg;

	return (arch_code_generator_t *)cg;
}


/*****************************************************************
 *  ____             _                  _   _____  _____
 * |  _ \           | |                | | |_   _|/ ____|  /\
 * | |_) | __ _  ___| | _____ _ __   __| |   | | | (___   /  \
 * |  _ < / _` |/ __| |/ / _ \ '_ \ / _` |   | |  \___ \ / /\ \
 * | |_) | (_| | (__|   <  __/ | | | (_| |  _| |_ ____) / ____ \
 * |____/ \__,_|\___|_|\_\___|_| |_|\__,_| |_____|_____/_/    \_\
 *
 *****************************************************************/

static mips_isa_t mips_isa_template = {
	{
		&mips_isa_if,
		&mips_gp_regs[REG_SP],
		&mips_gp_regs[REG_FP],
		&mips_reg_classes[CLASS_mips_gp],
		-1,		/* stack direction */
		2,      /* power of two stack alignment for calls, 2^2 == 4 */
		NULL,	/* main environment */
		7,      /* spill costs */
		5,      /* reload costs */
	},
	NULL,          /* cg */
};

/**
 * Initializes the backend ISA and opens the output file.
 */
static arch_env_t *mips_init(FILE *file_handle) {
	static int inited = 0;
	mips_isa_t *isa;

	if(inited)
		return NULL;
	inited = 1;

	isa = XMALLOC(mips_isa_t);
	memcpy(isa, &mips_isa_template, sizeof(isa[0]));

	be_emit_init(file_handle);

	mips_register_init();
	mips_create_opcodes(&mips_irn_ops);
	// mips_init_opcode_transforms();

	/* we mark referenced global entities, so we can only emit those which
	 * are actually referenced. (Note: you mustn't use the type visited flag
	 * elsewhere in the backend)
	 */
	inc_master_type_visited();

	return &isa->arch_env;
}

/**
 * Closes the output file and frees the ISA structure.
 */
static void mips_done(void *self)
{
	mips_isa_t *isa = self;

	be_gas_emit_decls(isa->arch_env.main_env, 1);

	be_emit_exit();
	free(isa);
}

static unsigned mips_get_n_reg_class(void)
{
	return N_CLASSES;
}

static const arch_register_class_t *mips_get_reg_class(unsigned i)
{
	assert(i < N_CLASSES);
	return &mips_reg_classes[i];
}



/**
 * Get the register class which shall be used to store a value of a given mode.
 * @param self The this pointer.
 * @param mode The mode in question.
 * @return A register class which can hold values of the given mode.
 */
const arch_register_class_t *mips_get_reg_class_for_mode(const ir_mode *mode)
{
	(void) mode;
	ASSERT_NO_FLOAT(mode);
	return &mips_reg_classes[CLASS_mips_gp];
}

typedef struct {
	be_abi_call_flags_bits_t flags;
	const arch_env_t *arch_env;
	ir_graph *irg;
	// do special handling to support debuggers
	int debug;
} mips_abi_env_t;

static void *mips_abi_init(const be_abi_call_t *call, const arch_env_t *arch_env, ir_graph *irg)
{
	mips_abi_env_t *env    = XMALLOC(mips_abi_env_t);
	be_abi_call_flags_t fl = be_abi_call_get_flags(call);
	env->flags             = fl.bits;
	env->irg               = irg;
	env->arch_env          = arch_env;
	env->debug             = 1;
	return env;
}

static const arch_register_t *mips_abi_prologue(void *self, ir_node** mem, pmap *reg_map, int *stack_bias)
{
	mips_abi_env_t *env = self;
	ir_graph *irg = env->irg;
	ir_node *block = get_irg_start_block(irg);
	ir_node *sp = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_SP]);
	ir_node *fp = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_FP]);
	int initialstackframesize;

	(void) stack_bias;

	if (env->debug) {
		/*
		 * The calling conventions wants a stack frame of at least 24bytes size with
		 *   a0-a3 saved in offset 0-12
		 *   fp saved in offset 16
		 *   ra saved in offset 20
		 */
		ir_node *mm[6];
		ir_node *sync, *reg, *store;
		initialstackframesize = 24;

		// - setup first part of stackframe
		sp = new_bd_mips_addu(NULL, block, sp,
		                      mips_create_Immediate(initialstackframesize));
		arch_set_irn_register(sp, &mips_gp_regs[REG_SP]);
		panic("FIXME Use IncSP or set register requirement with ignore");

		/* TODO: where to get an edge with a0-a3
		int i;
		for(i = 0; i < 4; ++i) {
			ir_node *reg = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_A0 + i]);
			ir_node *store = new_bd_mips_store_r(dbg, block, *mem, sp, reg, mode_T);
			attr = get_mips_attr(store);
			attr->load_store_mode = mode_Iu;
			attr->tv = new_tarval_from_long(i * 4, mode_Is);

			mm[i] = new_r_Proj(irg, block, store, mode_M, pn_Store_M);
		}
		*/

		reg = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_FP]);
		store = new_bd_mips_sw(NULL, block, sp, reg, *mem, NULL, 16);

		mm[4] = store;

		reg = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_RA]);
		store = new_bd_mips_sw(NULL, block, sp, reg, *mem, NULL, 20);

		mm[5] = store;

		/* Note: ideally we would route these mem edges directly towards the
		 * epilogue, but this is currently not supported so we sync all mems
		 * together */
		sync = new_r_Sync(block, 2, mm+4);
		*mem = sync;
	} else {
		ir_node *reg, *store;
		initialstackframesize = 4;

		// save old framepointer
		sp = new_bd_mips_addu(NULL, block, sp,
		                      mips_create_Immediate(-initialstackframesize));
		arch_set_irn_register(sp, &mips_gp_regs[REG_SP]);
		panic("FIXME Use IncSP or set register requirement with ignore");

		reg = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_FP]);
		store = new_bd_mips_sw(NULL, block, sp, reg, *mem, NULL, 0);

		*mem = store;
	}

	// setup framepointer
	fp = new_bd_mips_addu(NULL, block, sp,
	                      mips_create_Immediate(-initialstackframesize));
	arch_set_irn_register(fp, &mips_gp_regs[REG_FP]);
	panic("FIXME Use IncSP or set register requirement with ignore");

	be_abi_reg_map_set(reg_map, &mips_gp_regs[REG_FP], fp);
	be_abi_reg_map_set(reg_map, &mips_gp_regs[REG_SP], sp);

	return &mips_gp_regs[REG_SP];
}

static void mips_abi_epilogue(void *self, ir_node *block, ir_node **mem, pmap *reg_map)
{
	mips_abi_env_t   *env = self;

	ir_node *sp = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_SP]);
	ir_node *fp = be_abi_reg_map_get(reg_map, &mips_gp_regs[REG_FP]);
	ir_node *load;
	int initial_frame_size = env->debug ? 24 : 4;
	int fp_save_offset = env->debug ? 16 : 0;

	// copy fp to sp
	sp = new_bd_mips_or(NULL, block, fp, mips_create_zero());
	arch_set_irn_register(sp, &mips_gp_regs[REG_SP]);
	panic("FIXME Use be_Copy or set register requirement with ignore");

	// 1. restore fp
	load = new_bd_mips_lw(NULL, block, sp, *mem, NULL,
	                      fp_save_offset - initial_frame_size);
	panic("FIXME register requirement with ignore");

	fp = new_r_Proj(block, load, mode_Iu, pn_mips_lw_res);
	*mem = new_r_Proj(block, load, mode_Iu, pn_mips_lw_M);
	arch_set_irn_register(fp, &mips_gp_regs[REG_FP]);

	be_abi_reg_map_set(reg_map, &mips_gp_regs[REG_FP], fp);
	be_abi_reg_map_set(reg_map, &mips_gp_regs[REG_SP], sp);
}

/**
 * Produces the type which sits between the stack args and the locals on the stack.
 * it will contain the return address and space to store the old frame pointer.
 * @return The Firm type modelling the ABI between type.
 */
static ir_type *mips_abi_get_between_type(void *self) {
	mips_abi_env_t *env = self;

	static ir_type *debug_between_type = NULL;
	static ir_type *opt_between_type = NULL;
	static ir_entity *old_fp_ent    = NULL;

	if(env->debug && debug_between_type == NULL) {
		ir_entity *a0_ent, *a1_ent, *a2_ent, *a3_ent;
		ir_entity *ret_addr_ent;
		ir_type *ret_addr_type = new_type_primitive(mode_P);
		ir_type *old_fp_type   = new_type_primitive(mode_P);
		ir_type *old_param_type = new_type_primitive(mode_Iu);

		debug_between_type     = new_type_class(new_id_from_str("mips_between_type"));
		a0_ent				   = new_entity(debug_between_type, new_id_from_str("a0_ent"), old_param_type);
		a1_ent				   = new_entity(debug_between_type, new_id_from_str("a1_ent"), old_param_type);
		a2_ent				   = new_entity(debug_between_type, new_id_from_str("a2_ent"), old_param_type);
		a3_ent				   = new_entity(debug_between_type, new_id_from_str("a3_ent"), old_param_type);
		old_fp_ent             = new_entity(debug_between_type, new_id_from_str("old_fp"), old_fp_type);
		ret_addr_ent           = new_entity(debug_between_type, new_id_from_str("ret_addr"), ret_addr_type);

		set_entity_offset(a0_ent, 0);
		set_entity_offset(a1_ent, 4);
		set_entity_offset(a2_ent, 8);
		set_entity_offset(a3_ent, 12);
		set_entity_offset(old_fp_ent, 16);
		set_entity_offset(ret_addr_ent, 20);

		set_type_size_bytes(debug_between_type, 24);
	} else if(!env->debug && opt_between_type == NULL) {
		ir_type *old_fp_type   = new_type_primitive(mode_P);
		ir_entity *old_fp_ent;

		opt_between_type       = new_type_class(new_id_from_str("mips_between_type"));
		old_fp_ent             = new_entity(opt_between_type, new_id_from_str("old_fp"), old_fp_type);
		set_entity_offset(old_fp_ent, 0);
		set_type_size_bytes(opt_between_type, 4);
	}

	return env->debug ? debug_between_type : opt_between_type;
}

static const be_abi_callbacks_t mips_abi_callbacks = {
	mips_abi_init,
	free,
	mips_abi_get_between_type,
	mips_abi_prologue,
	mips_abi_epilogue,
};

/**
 * Get the ABI restrictions for procedure calls.
 * @param self        The this pointer.
 * @param method_type The type of the method (procedure) in question.
 * @param abi         The abi object to be modified
 */
static void mips_get_call_abi(const void *self, ir_type *method_type,
                              be_abi_call_t *abi)
{
	ir_type  *tp;
	ir_mode  *mode;
	int       n = get_method_n_params(method_type);
	int result_count;
	int       i;
	ir_mode **modes;
	const arch_register_t *reg;
	be_abi_call_flags_t call_flags;
	(void) self;

	memset(&call_flags, 0, sizeof(call_flags));
	call_flags.bits.left_to_right         = 0;
	call_flags.bits.store_args_sequential = 0;
	call_flags.bits.try_omit_fp           = 1;
	call_flags.bits.fp_free               = 0;
	call_flags.bits.call_has_imm          = 1;

	/* set stack parameter passing style */
	be_abi_call_set_flags(abi, call_flags, &mips_abi_callbacks);

	/* collect the mode for each type */
	modes = ALLOCAN(ir_mode*, n);
	for (i = 0; i < n; i++) {
		tp       = get_method_param_type(method_type, i);
		modes[i] = get_type_mode(tp);
	}

	// assigns parameters to registers or stack
	for (i = 0; i < n; i++) {
		// first 4 params in $a0-$a3, the others on the stack
		if(i < 4) {
			reg = &mips_gp_regs[REG_A0 + i];
			be_abi_call_param_reg(abi, i, reg);
		} else {
			/* default: all parameters on stack */
			be_abi_call_param_stack(abi, i, modes[i], 4, 0, 0);
		}
	}

	/* set return register */
	/* default: return value is in R0 (and maybe R1) */
	result_count = get_method_n_ress(method_type);
	assert(result_count <= 2 && "More than 2 result values not supported");
	for(i = 0; i < result_count; ++i) {
		const arch_register_t* reg;
		tp   = get_method_res_type(method_type, i);
		mode = get_type_mode(tp);
		ASSERT_NO_FLOAT(mode);

		reg = &mips_gp_regs[REG_V0 + i];
		be_abi_call_res_reg(abi, i, reg);
	}
}

/**
 * Initializes the code generator interface.
 */
static const arch_code_generator_if_t *mips_get_code_generator_if(void *self)
{
	(void) self;
	return &mips_code_gen_if;
}

/**
 * Returns the necessary byte alignment for storing a register of given class.
 */
static int mips_get_reg_class_alignment(const arch_register_class_t *cls)
{
	ir_mode *mode = arch_register_class_mode(cls);
	return get_mode_size_bytes(mode);
}

static const be_execution_unit_t ***mips_get_allowed_execution_units(
		const ir_node *irn)
{
	(void) irn;
	/* TODO */
	panic("Unimplemented mips_get_allowed_execution_units()");
}

static const be_machine_t *mips_get_machine(const void *self)
{
	(void) self;
	/* TODO */
	panic("Unimplemented mips_get_machine()");
}

/**
 * Return irp irgs in the desired order.
 */
static ir_graph **mips_get_irg_list(const void *self, ir_graph ***irg_list)
{
	(void) self;
	(void) irg_list;
	return NULL;
}

/**
 * Returns the libFirm configuration parameter for this backend.
 */
static const backend_params *mips_get_libfirm_params(void) {
	static backend_params p = {
		1,     /* need dword lowering */
		0,     /* don't support inline assembler yet */
		NULL,  /* will be set later */
		NULL,  /* but yet no creator function */
		NULL,  /* context for create_intrinsic_fkt */
		NULL,  /* no if conversion settings */
		NULL,  /* float arithmetic mode (TODO) */
		0,     /* no trampoline support: size 0 */
		0,     /* no trampoline support: align 0 */
		NULL,  /* no trampoline support: no trampoline builder */
		4      /* alignment of stack parameter */
	};

	return &p;
}

static asm_constraint_flags_t mips_parse_asm_constraint(const char **c)
{
	(void) c;
	return ASM_CONSTRAINT_FLAG_INVALID;
}

static int mips_is_valid_clobber(const char *clobber)
{
	(void) clobber;
	return 0;
}

const arch_isa_if_t mips_isa_if = {
	mips_init,
	mips_done,
	NULL,               /* handle intrinsics */
	mips_get_n_reg_class,
	mips_get_reg_class,
	mips_get_reg_class_for_mode,
	mips_get_call_abi,
	mips_get_code_generator_if,
	mips_get_list_sched_selector,
	mips_get_ilp_sched_selector,
	mips_get_reg_class_alignment,
	mips_get_libfirm_params,
	mips_get_allowed_execution_units,
	mips_get_machine,
	mips_get_irg_list,
	NULL,                /* mark remat */
	mips_parse_asm_constraint,
	mips_is_valid_clobber
};

void be_init_arch_mips(void)
{
	be_register_isa_if("mips", &mips_isa_if);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_mips);
