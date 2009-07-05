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
 * @brief   Handles fpu rounding modes
 * @author  Matthias Braun
 * @version $Id$
 *
 * The problem we deal with here is that the x86 ABI says the user can control
 * the fpu rounding mode, which means that when we do some operations like float
 * to int conversion which are specified as truncation in the C standard we have
 * to spill, change and restore the fpu rounding mode between spills.
 */
#include "config.h"

#include "ia32_fpu.h"
#include "ia32_new_nodes.h"
#include "ia32_architecture.h"
#include "gen_ia32_regalloc_if.h"

#include "ircons.h"
#include "irgwalk.h"
#include "tv.h"
#include "array.h"

#include "../beirgmod.h"
#include "../bearch_t.h"
#include "../besched.h"
#include "../beabi.h"
#include "../benode_t.h"
#include "../bestate.h"
#include "../beutil.h"
#include "../bessaconstr.h"
#include "../beirg_t.h"

static ir_entity *fpcw_round    = NULL;
static ir_entity *fpcw_truncate = NULL;

static ir_entity *create_ent(int value, const char *name)
{
	ir_mode   *mode = mode_Hu;
	ir_type   *type = new_type_primitive(new_id_from_str("_fpcw_type"), mode);
	ir_type   *glob = get_glob_type();
	ir_graph  *cnst_irg;
	ir_entity *ent;
	ir_node   *cnst;
	tarval    *tv;

	set_type_alignment_bytes(type, 4);

	tv  = new_tarval_from_long(value, mode);
	ent = new_entity(glob, new_id_from_str(name), type);
	set_entity_ld_ident(ent, get_entity_ident(ent));
	set_entity_visibility(ent, visibility_local);
	set_entity_variability(ent, variability_constant);
	set_entity_allocation(ent, allocation_static);

	cnst_irg = get_const_code_irg();
	cnst     = new_r_Const(cnst_irg, tv);
	set_atomic_ent_value(ent, cnst);

	return ent;
}

static void create_fpcw_entities(void)
{
	fpcw_round    = create_ent(0xc7f, "_fpcw_round");
	fpcw_truncate = create_ent(0x37f, "_fpcw_truncate");
}

static ir_node *create_fpu_mode_spill(void *env, ir_node *state, int force,
                                      ir_node *after)
{
	ia32_code_gen_t *cg = env;
	ir_node *spill = NULL;

	/* we don't spill the fpcw in unsafe mode */
	if(ia32_cg_config.use_unsafe_floatconv) {
		ir_node *block = get_nodes_block(state);
		if(force == 1 || !is_ia32_ChangeCW(state)) {
			ir_node *spill = new_bd_ia32_FnstCWNOP(NULL, block, state);
			sched_add_after(after, spill);
			return spill;
		}
		return NULL;
	}

	if(force == 1 || !is_ia32_ChangeCW(state)) {
		ir_graph *irg = get_irn_irg(state);
		ir_node *block = get_nodes_block(state);
		ir_node *noreg = ia32_new_NoReg_gp(cg);
		ir_node *nomem = new_NoMem();
		ir_node *frame = get_irg_frame(irg);

		spill = new_bd_ia32_FnstCW(NULL, block, frame, noreg, nomem, state);
		set_ia32_op_type(spill, ia32_AddrModeD);
		/* use mode_Iu, as movl has a shorter opcode than movw */
		set_ia32_ls_mode(spill, mode_Iu);
		set_ia32_use_frame(spill);

		sched_add_after(skip_Proj(after), spill);
	}

	return spill;
}

static ir_node *create_fldcw_ent(ia32_code_gen_t *cg, ir_node *block,
                                 ir_entity *entity)
{
	ir_node  *nomem = new_NoMem();
	ir_node  *noreg = ia32_new_NoReg_gp(cg);
	ir_node  *reload;

	reload = new_bd_ia32_FldCW(NULL, block, noreg, noreg, nomem);
	set_ia32_op_type(reload, ia32_AddrModeS);
	set_ia32_ls_mode(reload, ia32_reg_classes[CLASS_ia32_fp_cw].mode);
	set_ia32_am_sc(reload, entity);
	set_ia32_use_frame(reload);
	arch_set_irn_register(reload, &ia32_fp_cw_regs[REG_FPCW]);

	return reload;
}

static ir_node *create_fpu_mode_reload(void *env, ir_node *state,
                                       ir_node *spill, ir_node *before,
                                       ir_node *last_state)
{
	ia32_code_gen_t *cg    = env;
	ir_graph        *irg   = get_irn_irg(state);
	ir_node         *block = get_nodes_block(before);
	ir_node         *frame = get_irg_frame(irg);
	ir_node         *noreg = ia32_new_NoReg_gp(cg);
	ir_node         *reload = NULL;

	if(ia32_cg_config.use_unsafe_floatconv) {
		if(fpcw_round == NULL) {
			create_fpcw_entities();
		}
		if(spill != NULL) {
			reload = create_fldcw_ent(cg, block, fpcw_round);
		} else {
			reload = create_fldcw_ent(cg, block, fpcw_truncate);
		}
		sched_add_before(before, reload);
		return reload;
	}

	if(spill != NULL) {
		reload = new_bd_ia32_FldCW(NULL, block, frame, noreg, spill);
		set_ia32_op_type(reload, ia32_AddrModeS);
		set_ia32_ls_mode(reload, ia32_reg_classes[CLASS_ia32_fp_cw].mode);
		set_ia32_use_frame(reload);
		arch_set_irn_register(reload, &ia32_fp_cw_regs[REG_FPCW]);

		sched_add_before(before, reload);
	} else {
		ir_mode *lsmode = ia32_reg_classes[CLASS_ia32_fp_cw].mode;
		ir_node *nomem = new_NoMem();
		ir_node *cwstore, *load, *load_res, *or, *store, *fldcw;
		ir_node *or_const;

		assert(last_state != NULL);
		cwstore = new_bd_ia32_FnstCW(NULL, block, frame, noreg, nomem,
		                             last_state);
		set_ia32_op_type(cwstore, ia32_AddrModeD);
		set_ia32_ls_mode(cwstore, lsmode);
		set_ia32_use_frame(cwstore);
		sched_add_before(before, cwstore);

		load = new_bd_ia32_Load(NULL, block, frame, noreg, cwstore);
		set_ia32_op_type(load, ia32_AddrModeS);
		set_ia32_ls_mode(load, lsmode);
		set_ia32_use_frame(load);
		sched_add_before(before, load);

		load_res = new_r_Proj(block, load, mode_Iu, pn_ia32_Load_res);

		/* TODO: make the actual mode configurable in ChangeCW... */
		or_const = new_bd_ia32_Immediate(NULL, get_irg_start_block(irg),
		                                 NULL, 0, 0, 3072);
		arch_set_irn_register(or_const, &ia32_gp_regs[REG_GP_NOREG]);
		or = new_bd_ia32_Or(NULL, block, noreg, noreg, nomem, load_res,
		                    or_const);
		sched_add_before(before, or);

		store = new_bd_ia32_Store(NULL, block, frame, noreg, nomem, or);
		set_ia32_op_type(store, ia32_AddrModeD);
		/* use mode_Iu, as movl has a shorter opcode than movw */
		set_ia32_ls_mode(store, mode_Iu);
		set_ia32_use_frame(store);
		sched_add_before(before, store);

		fldcw = new_bd_ia32_FldCW(NULL, block, frame, noreg, store);
		set_ia32_op_type(fldcw, ia32_AddrModeS);
		set_ia32_ls_mode(fldcw, lsmode);
		set_ia32_use_frame(fldcw);
		arch_set_irn_register(fldcw, &ia32_fp_cw_regs[REG_FPCW]);
		sched_add_before(before, fldcw);

		reload = fldcw;
	}

	return reload;
}

typedef struct collect_fpu_mode_nodes_env_t {
	ir_node         **state_nodes;
} collect_fpu_mode_nodes_env_t;

static void collect_fpu_mode_nodes_walker(ir_node *node, void *data)
{
	collect_fpu_mode_nodes_env_t *env = data;
	const arch_register_t *reg;

	if(!mode_is_data(get_irn_mode(node)))
		return;

	reg = arch_get_irn_register(node);
	if(reg == &ia32_fp_cw_regs[REG_FPCW] && !is_ia32_ChangeCW(node)) {
		ARR_APP1(ir_node*, env->state_nodes, node);
	}
}

static
void rewire_fpu_mode_nodes(be_irg_t *birg)
{
	collect_fpu_mode_nodes_env_t env;
	be_ssa_construction_env_t senv;
	const arch_register_t *reg = &ia32_fp_cw_regs[REG_FPCW];
	ir_graph *irg = be_get_birg_irg(birg);
	ir_node *initial_value;
	ir_node **phis;
	be_lv_t *lv = be_get_birg_liveness(birg);
	int i, len;

	/* do ssa construction for the fpu modes */
	env.state_nodes = NEW_ARR_F(ir_node*, 0);
	irg_walk_graph(irg, collect_fpu_mode_nodes_walker, NULL, &env);

	initial_value = be_abi_get_ignore_irn(birg->abi, reg);

	/* nothing needs to be done, in fact we must not continue as for endless
	 * loops noone is using the initial_value and it will point to a bad node
	 * now
	 */
	if(ARR_LEN(env.state_nodes) == 0) {
		DEL_ARR_F(env.state_nodes);
		return;
	}

	be_ssa_construction_init(&senv, birg);
	be_ssa_construction_add_copies(&senv, env.state_nodes,
	                               ARR_LEN(env.state_nodes));
	be_ssa_construction_fix_users(&senv, initial_value);

	if(lv != NULL) {
		be_ssa_construction_update_liveness_phis(&senv, lv);
		be_liveness_update(lv, initial_value);
		len = ARR_LEN(env.state_nodes);
		for(i = 0; i < len; ++i) {
			be_liveness_update(lv, env.state_nodes[i]);
		}
	} else {
		be_liveness_invalidate(birg->lv);
	}

	/* set registers for the phis */
	phis = be_ssa_construction_get_new_phis(&senv);
	len = ARR_LEN(phis);
	for(i = 0; i < len; ++i) {
		ir_node *phi = phis[i];
		arch_set_irn_register(phi, reg);
	}
	be_ssa_construction_destroy(&senv);
	DEL_ARR_F(env.state_nodes);

	be_liveness_invalidate(be_get_birg_liveness(birg));
}

void ia32_setup_fpu_mode(ia32_code_gen_t *cg)
{
	/* do ssa construction for the fpu modes */
	rewire_fpu_mode_nodes(cg->birg);

	/* ensure correct fpu mode for operations */
	be_assure_state(cg->birg, &ia32_fp_cw_regs[REG_FPCW],
	                cg, create_fpu_mode_spill, create_fpu_mode_reload);
}
