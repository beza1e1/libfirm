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
 * @brief    The main TEMPLATE backend driver file.
 * @version  $Id$
 */
#include "config.h"

#include "pseudo_irg.h"
#include "irgwalk.h"
#include "irprog.h"
#include "irprintf.h"
#include "ircons.h"
#include "irgmod.h"

#include "bitset.h"
#include "debug.h"

#include "be.h"
#include "../bearch.h"
#include "../benode.h"
#include "../belower.h"
#include "../besched.h"
#include "../beabi.h"
#include "../bemodule.h"
#include "../begnuas.h"
#include "../belistsched.h"

#include "bearch_TEMPLATE_t.h"

#include "TEMPLATE_new_nodes.h"
#include "gen_TEMPLATE_regalloc_if.h"
#include "TEMPLATE_transform.h"
#include "TEMPLATE_emitter.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static arch_irn_class_t TEMPLATE_classify(const ir_node *irn)
{
	(void) irn;
	return 0;
}

static ir_entity *TEMPLATE_get_frame_entity(const ir_node *node)
{
	(void) node;
	/* TODO: return the ir_entity assigned to the frame */
	return NULL;
}

static void TEMPLATE_set_frame_entity(ir_node *node, ir_entity *ent)
{
	(void) node;
	(void) ent;
	/* TODO: set the ir_entity assigned to the frame */
}

/**
 * This function is called by the generic backend to correct offsets for
 * nodes accessing the stack.
 */
static void TEMPLATE_set_frame_offset(ir_node *irn, int offset)
{
	(void) irn;
	(void) offset;
	/* TODO: correct offset if irn accesses the stack */
}

static int TEMPLATE_get_sp_bias(const ir_node *irn)
{
	(void) irn;
	return 0;
}

/* fill register allocator interface */

static const arch_irn_ops_t TEMPLATE_irn_ops = {
	get_TEMPLATE_in_req,
	TEMPLATE_classify,
	TEMPLATE_get_frame_entity,
	TEMPLATE_set_frame_entity,
	TEMPLATE_set_frame_offset,
	TEMPLATE_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};



/**
 * Transforms the standard firm graph into
 * a TEMLPATE firm graph
 */
static void TEMPLATE_prepare_graph(void *self)
{
	TEMPLATE_code_gen_t *cg = self;

	irg_walk_blkwise_graph(cg->irg, NULL, TEMPLATE_transform_node, cg);
}



/**
 * Called immediatly before emit phase.
 */
static void TEMPLATE_finish_irg(void *self)
{
	TEMPLATE_code_gen_t *cg = self;
	ir_graph            *irg = cg->irg;

	dump_ir_block_graph_sched(irg, "-TEMPLATE-finished");
}


static void TEMPLATE_before_ra(void *self)
{
	(void) self;
	/* Some stuff you need to do after scheduling but before register allocation */
}

static void TEMPLATE_after_ra(void *self)
{
	(void) self;
	/* Some stuff you need to do immediatly after register allocation */
}



/**
 * Emits the code, closes the output file and frees
 * the code generator interface.
 */
static void TEMPLATE_emit_and_done(void *self)
{
	TEMPLATE_code_gen_t *cg = self;
	ir_graph           *irg = cg->irg;

	TEMPLATE_gen_routine(cg, irg);

	/* de-allocate code generator */
	free(cg);
}

static void *TEMPLATE_cg_init(be_irg_t *birg);

static const arch_code_generator_if_t TEMPLATE_code_gen_if = {
	TEMPLATE_cg_init,
	NULL,                    /* get_pic_base hook */
	NULL,                    /* before abi introduce hook */
	TEMPLATE_prepare_graph,
	NULL,                    /* spill hook */
	TEMPLATE_before_ra,      /* before register allocation hook */
	TEMPLATE_after_ra,       /* after register allocation hook */
	TEMPLATE_finish_irg,
	TEMPLATE_emit_and_done
};

/**
 * Initializes the code generator.
 */
static void *TEMPLATE_cg_init(be_irg_t *birg)
{
	const arch_env_t    *arch_env = be_get_birg_arch_env(birg);
	TEMPLATE_isa_t      *isa      = (TEMPLATE_isa_t *) arch_env;
	TEMPLATE_code_gen_t *cg       = XMALLOC(TEMPLATE_code_gen_t);

	cg->impl     = &TEMPLATE_code_gen_if;
	cg->irg      = be_get_birg_irg(birg);
	cg->isa      = isa;
	cg->birg     = birg;

	return (arch_code_generator_t *)cg;
}



const arch_isa_if_t TEMPLATE_isa_if;
static TEMPLATE_isa_t TEMPLATE_isa_template = {
	{
		&TEMPLATE_isa_if,             /* isa interface implementation */
		&TEMPLATE_gp_regs[REG_SP],  /* stack pointer register */
		&TEMPLATE_gp_regs[REG_BP],  /* base pointer register */
		&TEMPLATE_reg_classes[CLASS_TEMPLATE_gp],  /* link pointer register class */
		-1,                          /* stack direction */
		2,                           /* power of two stack alignment for calls, 2^2 == 4 */
		NULL,                        /* main environment */
		7,                           /* costs for a spill instruction */
		5,                           /* costs for a reload instruction */
	},
};

/**
 * Initializes the backend ISA
 */
static arch_env_t *TEMPLATE_init(FILE *outfile)
{
	static int run_once = 0;
	TEMPLATE_isa_t *isa;

	if (run_once)
		return NULL;
	run_once = 1;

	isa = XMALLOC(TEMPLATE_isa_t);
	memcpy(isa, &TEMPLATE_isa_template, sizeof(*isa));

	be_emit_init(outfile);

	TEMPLATE_register_init();
	TEMPLATE_create_opcodes(&TEMPLATE_irn_ops);

	return &isa->arch_env;
}



/**
 * Closes the output file and frees the ISA structure.
 */
static void TEMPLATE_done(void *self)
{
	TEMPLATE_isa_t *isa = self;

	/* emit now all global declarations */
	be_gas_emit_decls(isa->arch_env.main_env);

	be_emit_exit();
	free(self);
}


static unsigned TEMPLATE_get_n_reg_class(void)
{
	return N_CLASSES;
}

static const arch_register_class_t *TEMPLATE_get_reg_class(unsigned i)
{
	assert(i < N_CLASSES);
	return &TEMPLATE_reg_classes[i];
}



/**
 * Get the register class which shall be used to store a value of a given mode.
 * @param self The this pointer.
 * @param mode The mode in question.
 * @return A register class which can hold values of the given mode.
 */
static const arch_register_class_t *TEMPLATE_get_reg_class_for_mode(const ir_mode *mode)
{
	if (mode_is_float(mode))
		return &TEMPLATE_reg_classes[CLASS_TEMPLATE_fp];
	else
		return &TEMPLATE_reg_classes[CLASS_TEMPLATE_gp];
}



typedef struct {
	be_abi_call_flags_bits_t flags;
	const arch_env_t *arch_env;
	ir_graph *irg;
} TEMPLATE_abi_env_t;

static void *TEMPLATE_abi_init(const be_abi_call_t *call, const arch_env_t *arch_env, ir_graph *irg)
{
	TEMPLATE_abi_env_t *env = XMALLOC(TEMPLATE_abi_env_t);
	be_abi_call_flags_t fl = be_abi_call_get_flags(call);
	env->flags    = fl.bits;
	env->irg      = irg;
	env->arch_env = arch_env;
	return env;
}

/**
 * Get the between type for that call.
 * @param self The callback object.
 * @return The between type of for that call.
 */
static ir_type *TEMPLATE_get_between_type(void *self)
{
	static ir_type *between_type = NULL;
	static ir_entity *old_bp_ent = NULL;
	(void) self;

	if (!between_type) {
		ir_entity *ret_addr_ent;
		ir_type *ret_addr_type = new_type_primitive(mode_P);
		ir_type *old_bp_type   = new_type_primitive(mode_P);

		between_type           = new_type_class(new_id_from_str("TEMPLATE_between_type"));
		old_bp_ent             = new_entity(between_type, new_id_from_str("old_bp"), old_bp_type);
		ret_addr_ent           = new_entity(between_type, new_id_from_str("old_bp"), ret_addr_type);

		set_entity_offset(old_bp_ent, 0);
		set_entity_offset(ret_addr_ent, get_type_size_bytes(old_bp_type));
		set_type_size_bytes(between_type, get_type_size_bytes(old_bp_type) + get_type_size_bytes(ret_addr_type));
	}

	return between_type;
}

/**
 * Build the prolog, return the BASE POINTER register
 */
static const arch_register_t *TEMPLATE_abi_prologue(void *self, ir_node **mem,
                                                    pmap *reg_map, int *stack_bias)
{
	TEMPLATE_abi_env_t *env = self;
	(void) reg_map;
	(void) mem;
	(void) stack_bias;

	if (env->flags.try_omit_fp)
		return env->arch_env->sp;
	return env->arch_env->bp;
}

/* Build the epilog */
static void TEMPLATE_abi_epilogue(void *self, ir_node *bl, ir_node **mem,
                                  pmap *reg_map)
{
	(void) self;
	(void) bl;
	(void) mem;
	(void) reg_map;
}

static const be_abi_callbacks_t TEMPLATE_abi_callbacks = {
	TEMPLATE_abi_init,
	free,
	TEMPLATE_get_between_type,
	TEMPLATE_abi_prologue,
	TEMPLATE_abi_epilogue,
};

/**
 * Get the ABI restrictions for procedure calls.
 * @param self        The this pointer.
 * @param method_type The type of the method (procedure) in question.
 * @param abi         The abi object to be modified
 */
static void TEMPLATE_get_call_abi(const void *self, ir_type *method_type,
                                  be_abi_call_t *abi)
{
	ir_type  *tp;
	ir_mode  *mode;
	int       i, n = get_method_n_params(method_type);
	be_abi_call_flags_t call_flags;
	(void) self;

	/* set abi flags for calls */
	call_flags.bits.left_to_right         = 0;
	call_flags.bits.store_args_sequential = 1;
	call_flags.bits.try_omit_fp           = 1;
	call_flags.bits.fp_free               = 0;
	call_flags.bits.call_has_imm          = 1;

	/* set stack parameter passing style */
	be_abi_call_set_flags(abi, call_flags, &TEMPLATE_abi_callbacks);

	for (i = 0; i < n; i++) {
		/* TODO: implement register parameter: */
		/* reg = get reg for param i;          */
		/* be_abi_call_param_reg(abi, i, reg, ABI_CONTEXT_BOTH); */

		/* default: all parameters on stack */
		tp   = get_method_param_type(method_type, i);
		mode = get_type_mode(tp);
		be_abi_call_param_stack(abi, i, mode, 4, 0, 0, ABI_CONTEXT_BOTH);
	}

	/* TODO: set correct return register */
	/* default: return value is in R0 resp. F0 */
	if (get_method_n_ress(method_type) > 0) {
		tp   = get_method_res_type(method_type, 0);
		mode = get_type_mode(tp);

		be_abi_call_res_reg(abi, 0,
			mode_is_float(mode) ? &TEMPLATE_fp_regs[REG_F0] : &TEMPLATE_gp_regs[REG_R0], ABI_CONTEXT_BOTH);
	}
}

static int TEMPLATE_to_appear_in_schedule(void *block_env, const ir_node *irn)
{
	(void) block_env;

	if (!is_TEMPLATE_irn(irn))
		return -1;

	return 1;
}

/**
 * Initializes the code generator interface.
 */
static const arch_code_generator_if_t *TEMPLATE_get_code_generator_if(
		void *self)
{
	(void) self;
	return &TEMPLATE_code_gen_if;
}

list_sched_selector_t TEMPLATE_sched_selector;

/**
 * Returns the reg_pressure scheduler with to_appear_in_schedule() overloaded
 */
static const list_sched_selector_t *TEMPLATE_get_list_sched_selector(
		const void *self, list_sched_selector_t *selector)
{
	(void) self;
	(void) selector;

	TEMPLATE_sched_selector = trivial_selector;
	TEMPLATE_sched_selector.to_appear_in_schedule = TEMPLATE_to_appear_in_schedule;
	return &TEMPLATE_sched_selector;
}

static const ilp_sched_selector_t *TEMPLATE_get_ilp_sched_selector(
		const void *self)
{
	(void) self;
	return NULL;
}

/**
 * Returns the necessary byte alignment for storing a register of given class.
 */
static int TEMPLATE_get_reg_class_alignment(const arch_register_class_t *cls)
{
	ir_mode *mode = arch_register_class_mode(cls);
	return get_mode_size_bytes(mode);
}

/**
 * Returns the libFirm configuration parameter for this backend.
 */
static const backend_params *TEMPLATE_get_backend_params(void)
{
	static backend_params p = {
		0,     /* no dword lowering */
		0,     /* no inline assembly */
		NULL,  /* will be set later */
		NULL,  /* no creator function */
		NULL,  /* context for create_intrinsic_fkt */
		NULL,  /* parameter for if conversion */
		NULL,  /* float arithmetic mode */
		0,     /* no trampoline support: size 0 */
		0,     /* no trampoline support: align 0 */
		NULL,  /* no trampoline support: no trampoline builder */
		4      /* alignment of stack parameter: typically 4 (32bit) or 8 (64bit) */
	};
	return &p;
}

static const be_execution_unit_t ***TEMPLATE_get_allowed_execution_units(
		const ir_node *irn)
{
	(void) irn;
	/* TODO */
	return NULL;
}

static const be_machine_t *TEMPLATE_get_machine(const void *self)
{
	(void) self;
	/* TODO */
	return NULL;
}

static ir_graph **TEMPLATE_get_backend_irg_list(const void *self,
                                                ir_graph ***irgs)
{
	(void) self;
	(void) irgs;
	return NULL;
}

static asm_constraint_flags_t TEMPLATE_parse_asm_constraint(const char **c)
{
	(void) c;
	return ASM_CONSTRAINT_FLAG_INVALID;
}

static int TEMPLATE_is_valid_clobber(const char *clobber)
{
	(void) clobber;
	return 0;
}

const arch_isa_if_t TEMPLATE_isa_if = {
	TEMPLATE_init,
	TEMPLATE_done,
	NULL,                /* handle intrinsics */
	TEMPLATE_get_n_reg_class,
	TEMPLATE_get_reg_class,
	TEMPLATE_get_reg_class_for_mode,
	TEMPLATE_get_call_abi,
	TEMPLATE_get_code_generator_if,
	TEMPLATE_get_list_sched_selector,
	TEMPLATE_get_ilp_sched_selector,
	TEMPLATE_get_reg_class_alignment,
    TEMPLATE_get_backend_params,
	TEMPLATE_get_allowed_execution_units,
	TEMPLATE_get_machine,
	TEMPLATE_get_backend_irg_list,
	NULL,                    /* mark remat */
	TEMPLATE_parse_asm_constraint,
	TEMPLATE_is_valid_clobber
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_TEMPLATE);
void be_init_arch_TEMPLATE(void)
{
	be_register_isa_if("TEMPLATE", &TEMPLATE_isa_if);
	FIRM_DBG_REGISTER(dbg, "firm.be.TEMPLATE.cg");
	TEMPLATE_init_transform();
}
