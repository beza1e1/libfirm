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
 * @brief   This file implements the creation of the achitecture specific firm
 *          opcodes and the coresponding node constructors for the sparc
 *          assembler irg.
 * @version $Id$
 */
#include "config.h"

#include <stdlib.h>

#include "irprog_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "ircons_t.h"
#include "iropt_t.h"
#include "irop.h"
#include "irprintf.h"
#include "xmalloc.h"

#include "../bearch.h"

#include "sparc_nodes_attr.h"
#include "sparc_new_nodes.h"
#include "gen_sparc_regalloc_if.h"

bool sparc_has_load_store_attr(const ir_node *node)
{
	return is_sparc_Ld(node) || is_sparc_St(node) || is_sparc_Ldf(node)
	    || is_sparc_Stf(node);
}

static bool has_jmp_cond_attr(const ir_node *node)
{
	return is_sparc_Bicc(node) || is_sparc_fbfcc(node);
}

static bool has_switch_jmp_attr(const ir_node *node)
{
	return is_sparc_SwitchJmp(node);
}

static bool has_save_attr(const ir_node *node)
{
	return is_sparc_Save(node);
}

static bool has_fp_attr(const ir_node *node)
{
	return is_sparc_fadd(node) || is_sparc_fsub(node)
	    || is_sparc_fmul(node) || is_sparc_fdiv(node)
	    || is_sparc_fftoi(node) || is_sparc_fitof(node)
	    || is_sparc_fneg(node) || is_sparc_fcmp(node);
}

static bool has_fp_conv_attr(const ir_node *node)
{
	return is_sparc_fftof(node);
}

/**
 * Dumper interface for dumping sparc nodes in vcg.
 * @param F        the output file
 * @param n        the node to dump
 * @param reason   indicates which kind of information should be dumped
 */
static void sparc_dump_node(FILE *F, ir_node *n, dump_reason_t reason)
{
	const sparc_attr_t *attr;

	switch (reason) {
	case dump_node_opcode_txt:
		fprintf(F, "%s", get_irn_opname(n));
		break;

	case dump_node_mode_txt:
		break;

	case dump_node_info_txt:
		arch_dump_reqs_and_registers(F, n);
		attr = get_sparc_attr_const(n);
		if (attr->immediate_value_entity) {
			ir_fprintf(F, "entity: %+F (offset %d)\n",
			           attr->immediate_value_entity, attr->immediate_value);
		} else {
			ir_fprintf(F, "immediate value: %d\n", attr->immediate_value);
		}
		if (has_save_attr(n)) {
			const sparc_save_attr_t *attr = get_sparc_save_attr_const(n);
			fprintf(F, "initial stacksize: %d\n", attr->initial_stacksize);
		}
		if (sparc_has_load_store_attr(n)) {
			const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(n);
			ir_fprintf(F, "load store mode: %+F\n", attr->load_store_mode);
			fprintf(F, "is frame entity: %s\n",
			        attr->is_frame_entity ? "true" : "false");
		}
		if (has_jmp_cond_attr(n)) {
			const sparc_jmp_cond_attr_t *attr
				= get_sparc_jmp_cond_attr_const(n);
			fprintf(F, "pnc: %d (%s)\n", attr->pnc, get_pnc_string(attr->pnc));
			fprintf(F, "unsigned: %s\n", attr->is_unsigned ? "true" : "false");
		}
		if (has_switch_jmp_attr(n)) {
			const sparc_switch_jmp_attr_t *attr
				= get_sparc_switch_jmp_attr_const(n);
			fprintf(F, "default proj: %ld\n", attr->default_proj_num);
		}
		if (has_fp_attr(n)) {
			const sparc_fp_attr_t *attr = get_sparc_fp_attr_const(n);
			ir_fprintf(F, "fp_mode: %+F\n", attr->fp_mode);
		}
		if (has_fp_conv_attr(n)) {
			const sparc_fp_conv_attr_t *attr = get_sparc_fp_conv_attr_const(n);
			ir_fprintf(F, "conv from: %+F\n", attr->src_mode);
			ir_fprintf(F, "conv to: %+F\n", attr->dest_mode);
		}
		break;

	case dump_node_nodeattr_txt:
		break;
	}
}

static void sparc_set_attr_imm(ir_node *res, ir_entity *entity,
                               int32_t immediate_value)
{
	sparc_attr_t *attr           = get_irn_generic_attr(res);
	attr->immediate_value_entity = entity;
	attr->immediate_value        = immediate_value;
}

static void init_sparc_jmp_cond_attr(ir_node *node, int pnc, bool is_unsigned)
{
	sparc_jmp_cond_attr_t *attr = get_sparc_jmp_cond_attr(node);
	attr->pnc         = pnc;
	attr->is_unsigned = is_unsigned;
}

sparc_attr_t *get_sparc_attr(ir_node *node)
{
	assert(is_sparc_irn(node));
	return (sparc_attr_t*) get_irn_generic_attr(node);
}

const sparc_attr_t *get_sparc_attr_const(const ir_node *node)
{
	assert(is_sparc_irn(node));
	return (const sparc_attr_t*) get_irn_generic_attr_const(node);
}

sparc_load_store_attr_t *get_sparc_load_store_attr(ir_node *node)
{
	assert(sparc_has_load_store_attr(node));
	return (sparc_load_store_attr_t*) get_irn_generic_attr_const(node);
}

const sparc_load_store_attr_t *get_sparc_load_store_attr_const(const ir_node *node)
{
	assert(sparc_has_load_store_attr(node));
	return (const sparc_load_store_attr_t*) get_irn_generic_attr_const(node);
}

sparc_jmp_cond_attr_t *get_sparc_jmp_cond_attr(ir_node *node)
{
	assert(has_jmp_cond_attr(node));
	return (sparc_jmp_cond_attr_t*) get_irn_generic_attr_const(node);
}

const sparc_jmp_cond_attr_t *get_sparc_jmp_cond_attr_const(const ir_node *node)
{
	assert(has_jmp_cond_attr(node));
	return (const sparc_jmp_cond_attr_t*) get_irn_generic_attr_const(node);
}

sparc_switch_jmp_attr_t *get_sparc_switch_jmp_attr(ir_node *node)
{
	assert(has_switch_jmp_attr(node));
	return (sparc_switch_jmp_attr_t*) get_irn_generic_attr_const(node);
}

const sparc_switch_jmp_attr_t *get_sparc_switch_jmp_attr_const(const ir_node *node)
{
	assert(has_switch_jmp_attr(node));
	return (const sparc_switch_jmp_attr_t*) get_irn_generic_attr_const(node);
}

sparc_save_attr_t *get_sparc_save_attr(ir_node *node)
{
	assert(has_save_attr(node));
	return (sparc_save_attr_t*) get_irn_generic_attr_const(node);
}

const sparc_save_attr_t *get_sparc_save_attr_const(const ir_node *node)
{
	assert(has_save_attr(node));
	return (const sparc_save_attr_t*) get_irn_generic_attr_const(node);
}

sparc_fp_attr_t *get_sparc_fp_attr(ir_node *node)
{
	assert(has_fp_attr(node));
	return (sparc_fp_attr_t*) get_irn_generic_attr(node);
}

const sparc_fp_attr_t *get_sparc_fp_attr_const(const ir_node *node)
{
	assert(has_fp_attr(node));
	return (const sparc_fp_attr_t*) get_irn_generic_attr_const(node);
}

sparc_fp_conv_attr_t *get_sparc_fp_conv_attr(ir_node *node)
{
	assert(has_fp_conv_attr(node));
	return (sparc_fp_conv_attr_t*) get_irn_generic_attr(node);
}

const sparc_fp_conv_attr_t *get_sparc_fp_conv_attr_const(const ir_node *node)
{
	assert(has_fp_conv_attr(node));
	return (const sparc_fp_conv_attr_t*) get_irn_generic_attr_const(node);
}

/**
 * Returns the argument register requirements of a sparc node.
 */
const arch_register_req_t **get_sparc_in_req_all(const ir_node *node)
{
	const sparc_attr_t *attr = get_sparc_attr_const(node);
	return attr->in_req;
}

void set_sparc_in_req_all(ir_node *node, const arch_register_req_t **reqs)
{
	sparc_attr_t *attr = get_sparc_attr(node);
	attr->in_req = reqs;
}

/**
 * Returns the argument register requirement at position pos of an sparc node.
 */
const arch_register_req_t *get_sparc_in_req(const ir_node *node, int pos)
{
	const sparc_attr_t *attr = get_sparc_attr_const(node);
	return attr->in_req[pos];
}

/**
 * Sets the IN register requirements at position pos.
 */
void set_sparc_req_in(ir_node *node, const arch_register_req_t *req, int pos)
{
	sparc_attr_t *attr  = get_sparc_attr(node);
	attr->in_req[pos] = req;
}

/**
 * Initializes the nodes attributes.
 */
static void init_sparc_attributes(ir_node *node, arch_irn_flags_t flags,
                                  const arch_register_req_t **in_reqs,
                                  const be_execution_unit_t ***execution_units,
                                  int n_res)
{
	ir_graph        *irg  = get_irn_irg(node);
	struct obstack  *obst = get_irg_obstack(irg);
	sparc_attr_t *attr = get_sparc_attr(node);
	backend_info_t  *info;
	(void) execution_units;

	arch_irn_set_flags(node, flags);
	attr->in_req = in_reqs;

	info            = be_get_info(node);
	info->out_infos = NEW_ARR_D(reg_out_info_t, obst, n_res);
	memset(info->out_infos, 0, n_res * sizeof(info->out_infos[0]));
}

static void init_sparc_load_store_attributes(ir_node *res, ir_mode *ls_mode,
											ir_entity *entity, int32_t offset,
											bool is_frame_entity,
											bool is_reg_reg)
{
	sparc_load_store_attr_t *attr     = get_sparc_load_store_attr(res);
	attr->base.immediate_value_entity = entity;
	attr->base.immediate_value        = offset;
	attr->load_store_mode             = ls_mode;
	attr->is_frame_entity             = is_frame_entity;
	attr->is_reg_reg                  = is_reg_reg;
}

static void init_sparc_save_attributes(ir_node *res, int initial_stacksize)
{
	sparc_save_attr_t *attr = get_sparc_save_attr(res);
	attr->initial_stacksize = initial_stacksize;
}

static void init_sparc_fp_attributes(ir_node *res, ir_mode *fp_mode)
{
	sparc_fp_attr_t *attr = get_sparc_fp_attr(res);
	attr->fp_mode = fp_mode;
}

static void init_sparc_fp_conv_attributes(ir_node *res, ir_mode *src_mode,
                                          ir_mode *dest_mode)
{
	sparc_fp_conv_attr_t *attr = get_sparc_fp_conv_attr(res);
	attr->src_mode = src_mode;
	attr->dest_mode = dest_mode;
}

static void init_sparc_switch_jmp_attributes(ir_node *res, long default_pn,
                                             ir_entity *jump_table)
{
	sparc_switch_jmp_attr_t *attr = get_sparc_switch_jmp_attr(res);
	attr->default_proj_num = default_pn;
	attr->jump_table       = jump_table;
}

/**
 * copies sparc attributes of  node
 */
static void sparc_copy_attr(ir_graph *irg, const ir_node *old_node,
                            ir_node *new_node)
{
	struct obstack     *obst    = get_irg_obstack(irg);
	const sparc_attr_t *attr_old = get_sparc_attr_const(old_node);
	sparc_attr_t       *attr_new = get_sparc_attr(new_node);
	backend_info_t     *old_info = be_get_info(old_node);
	backend_info_t     *new_info = be_get_info(new_node);

	/* copy the attributes */
	memcpy(attr_new, attr_old, get_op_attr_size(get_irn_op(old_node)));
	/* copy out flags */
	new_info->out_infos =
		DUP_ARR_D(reg_out_info_t, obst, old_info->out_infos);
}

/**
 * compare some node's attributes
 */
static int cmp_attr_sparc(ir_node *a, ir_node *b)
{
	const sparc_attr_t *attr_a = get_sparc_attr_const(a);
	const sparc_attr_t *attr_b = get_sparc_attr_const(b);

	return attr_a->immediate_value != attr_b->immediate_value
		|| attr_a->immediate_value_entity != attr_b->immediate_value_entity;
}

static int cmp_attr_sparc_load_store(ir_node *a, ir_node *b)
{
	const sparc_load_store_attr_t *attr_a = get_sparc_load_store_attr_const(a);
	const sparc_load_store_attr_t *attr_b = get_sparc_load_store_attr_const(b);

	if (cmp_attr_sparc(a, b))
		return 1;

	return attr_a->is_frame_entity != attr_b->is_frame_entity
			|| attr_a->load_store_mode != attr_b->load_store_mode;
}

static int cmp_attr_sparc_jmp_cond(ir_node *a, ir_node *b)
{
	const sparc_jmp_cond_attr_t *attr_a = get_sparc_jmp_cond_attr_const(a);
	const sparc_jmp_cond_attr_t *attr_b = get_sparc_jmp_cond_attr_const(b);

	if (cmp_attr_sparc(a, b))
		return 1;

	return attr_a->pnc != attr_b->pnc
	    || attr_a->is_unsigned != attr_b->is_unsigned;
}

static int cmp_attr_sparc_switch_jmp(ir_node *a, ir_node *b)
{
	const sparc_switch_jmp_attr_t *attr_a = get_sparc_switch_jmp_attr_const(a);
	const sparc_switch_jmp_attr_t *attr_b = get_sparc_switch_jmp_attr_const(b);

	if (cmp_attr_sparc(a, b))
		return 1;

	return attr_a->default_proj_num != attr_b->default_proj_num;
}

static int cmp_attr_sparc_save(ir_node *a, ir_node *b)
{
	const sparc_save_attr_t *attr_a = get_sparc_save_attr_const(a);
	const sparc_save_attr_t *attr_b = get_sparc_save_attr_const(b);

	if (cmp_attr_sparc(a, b))
		return 1;

	return attr_a->initial_stacksize != attr_b->initial_stacksize;
}

static int cmp_attr_sparc_fp(ir_node *a, ir_node *b)
{
	const sparc_fp_attr_t *attr_a = get_sparc_fp_attr_const(a);
	const sparc_fp_attr_t *attr_b = get_sparc_fp_attr_const(b);

	if (cmp_attr_sparc(a, b))
		return 1;

	return attr_a->fp_mode != attr_b->fp_mode;
}

static int cmp_attr_sparc_fp_conv(ir_node *a, ir_node *b)
{
	const sparc_fp_conv_attr_t *attr_a = get_sparc_fp_conv_attr_const(a);
	const sparc_fp_conv_attr_t *attr_b = get_sparc_fp_conv_attr_const(b);

	if (cmp_attr_sparc(a, b))
		return 1;

	return attr_a->src_mode != attr_b->src_mode
	    || attr_a->dest_mode != attr_b->dest_mode;;
}

/* Include the generated constructor functions */
#include "gen_sparc_new_nodes.c.inl"
