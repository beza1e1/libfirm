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
 * @brief   code selection (transform FIRM into TEMPLATE FIRM)
 * @version $Id$
 */
#include "config.h"

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irgmod.h"
#include "iredges.h"
#include "ircons.h"
#include "iropt_t.h"
#include "debug.h"
#include "error.h"

#include "../benode.h"
#include "../betranshlp.h"
#include "bearch_TEMPLATE_t.h"

#include "TEMPLATE_nodes_attr.h"
#include "TEMPLATE_transform.h"
#include "TEMPLATE_new_nodes.h"

#include "gen_TEMPLATE_regalloc_if.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

typedef ir_node* (*new_binop_func)(dbg_info *dbgi, ir_node *block,
                                   ir_node *left, ir_node *right);

static ir_node *transform_binop(ir_node *node, new_binop_func new_func)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *left      = get_binop_left(node);
	ir_node  *new_left  = be_transform_node(left);
	ir_node  *right     = get_binop_right(node);
	ir_node  *new_right = be_transform_node(right);

	return new_func(dbgi, new_block, new_left, new_right);
}

static ir_node *gen_And(ir_node *node)
{
	return transform_binop(node, new_bd_TEMPLATE_And);
}

static ir_node *gen_Or(ir_node *node)
{
	return transform_binop(node, new_bd_TEMPLATE_Or);
}

static ir_node *gen_Eor(ir_node *node)
{
	return transform_binop(node, new_bd_TEMPLATE_Xor);
}

static ir_node *gen_Div(ir_node *node)
{
	ir_mode *mode = get_Div_resmode(node);
	assert(mode_is_float(mode));
	return transform_binop(node, new_bd_TEMPLATE_fDiv);
}

static ir_node *gen_Shl(ir_node *node)
{
	return transform_binop(node, new_bd_TEMPLATE_Shl);
}

static ir_node *gen_Shr(ir_node *node)
{
	return transform_binop(node, new_bd_TEMPLATE_Shr);
}

static ir_node *gen_Add(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return transform_binop(node, new_bd_TEMPLATE_fAdd);
	}
	return transform_binop(node, new_bd_TEMPLATE_Add);
}

static ir_node *gen_Sub(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return transform_binop(node, new_bd_TEMPLATE_fSub);
	}
	return transform_binop(node, new_bd_TEMPLATE_Sub);
}

static ir_node *gen_Mul(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return transform_binop(node, new_bd_TEMPLATE_fMul);
	}
	return transform_binop(node, new_bd_TEMPLATE_Mul);
}


typedef ir_node* (*new_unop_func)(dbg_info *dbgi, ir_node *block, ir_node *op);

static ir_node *transform_unop(ir_node *node, new_unop_func new_func)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *op        = get_unop_op(node);
	ir_node  *new_op    = be_transform_node(op);

	return new_func(dbgi, new_block, new_op);
}

static ir_node *gen_Minus(ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return transform_unop(node, new_bd_TEMPLATE_fMinus);
	}
	return transform_unop(node, new_bd_TEMPLATE_Minus);
}

static ir_node *gen_Not(ir_node *node)
{
	return transform_unop(node, new_bd_TEMPLATE_Not);
}

static ir_node *gen_Const(ir_node *node)
{
	ir_node   *block     = get_nodes_block(node);
	ir_node   *new_block = be_transform_node(block);
	dbg_info  *dbgi      = get_irn_dbg_info(node);
	ir_tarval *value     = get_Const_tarval(node);
	ir_node   *result;

	result = new_bd_TEMPLATE_Const(dbgi, new_block, value);

	/* make sure the node does not float above the barrier into the prologue */
	be_dep_on_frame(result);

	return result;
}

static ir_node *gen_Load(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *ptr       = get_Load_ptr(node);
	ir_node  *new_ptr   = be_transform_node(ptr);
	ir_node  *mem       = get_Load_mem(node);
	ir_node  *new_mem   = be_transform_node(mem);
	ir_mode  *mode      = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return new_bd_TEMPLATE_fLoad(dbgi, new_block, new_ptr, new_mem, mode);
	}
	return new_bd_TEMPLATE_Load(dbgi, new_block, new_ptr, new_mem, mode);
}

static ir_node *gen_Store(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *ptr       = get_Store_ptr(node);
	ir_node  *new_ptr   = be_transform_node(ptr);
	ir_node  *val       = get_Store_value(node);
	ir_node  *new_val   = be_transform_node(val);
	ir_node  *mem       = get_Store_mem(node);
	ir_node  *new_mem   = be_transform_node(mem);
	ir_mode  *mode      = get_irn_mode(node);

	if (mode_is_float(mode)) {
		return new_bd_TEMPLATE_fStore(dbgi, new_block, new_ptr, new_val, new_mem, mode);
	}
	return new_bd_TEMPLATE_Store(dbgi, new_block, new_ptr, new_mem, new_val, mode);
}

static ir_node *gen_Jmp(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);

	return new_bd_TEMPLATE_Jmp(dbgi, new_block);
}

/**
 * returns true if mode should be stored in a general purpose register
 */
static inline bool mode_needs_gp_reg(ir_mode *mode)
{
	return mode_is_int(mode) || mode_is_reference(mode);
}

static ir_node *gen_Phi(ir_node *node)
{
	const arch_register_req_t *req;
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_mode  *mode      = get_irn_mode(node);
	ir_graph *irg       = get_irn_irg(node);
	ir_node  *phi;

	if (mode_needs_gp_reg(mode)) {
		mode = mode_Iu;
		req  = TEMPLATE_reg_classes[CLASS_TEMPLATE_gp].class_req;
	} else {
		req = arch_no_register_req;
	}

	phi = new_ir_node(dbgi, irg, new_block, op_Phi, mode, get_irn_arity(node),
	                  get_irn_in(node)+1);
	copy_node_attr(irg, node, phi);
	be_duplicate_deps(node, phi);

	arch_set_out_register_req(phi, 0, req);
	be_enqueue_preds(node);
	return phi;
}

static void TEMPLATE_register_transformers(void)
{
	be_start_transform_setup();

	be_set_transform_function(op_Add,   gen_Add);
	be_set_transform_function(op_And,   gen_And);
	be_set_transform_function(op_Const, gen_Const);
	be_set_transform_function(op_Div,   gen_Div);
	be_set_transform_function(op_Eor,   gen_Eor);
	be_set_transform_function(op_Jmp,   gen_Jmp);
	be_set_transform_function(op_Load,  gen_Load);
	be_set_transform_function(op_Minus, gen_Minus);
	be_set_transform_function(op_Mul,   gen_Mul);
	be_set_transform_function(op_Not,   gen_Not);
	be_set_transform_function(op_Or,    gen_Or);
	be_set_transform_function(op_Phi,   gen_Phi);
	be_set_transform_function(op_Shl,   gen_Shl);
	be_set_transform_function(op_Shr,   gen_Shr);
	be_set_transform_function(op_Store, gen_Store);
	be_set_transform_function(op_Sub,   gen_Sub);
}

/**
 * Transform generic IR-nodes into TEMPLATE machine instructions
 */
void TEMPLATE_transform_graph(ir_graph *irg)
{
	TEMPLATE_register_transformers();
	be_transform_graph(irg, NULL);
}

void TEMPLATE_init_transform(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.TEMPLATE.transform");
}
