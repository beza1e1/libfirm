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
 * @brief       Processor architecture specification.
 * @author      Sebastian Hack
 * @version     $Id$
 */
#include "config.h"

#include <string.h>

#include "bearch.h"
#include "benode.h"
#include "beinfo.h"
#include "ircons_t.h"
#include "irnode_t.h"
#include "irop_t.h"

#include "bitset.h"
#include "pset.h"
#include "raw_bitset.h"

#include "irprintf.h"

/* Initialize the architecture environment struct. */
arch_env_t *arch_env_init(const arch_isa_if_t *isa_if, FILE *file_handle, be_main_env_t *main_env)
{
	arch_env_t *arch_env = isa_if->init(file_handle);
	arch_env->main_env   = main_env;
	return arch_env;
}

/**
 * Get the isa responsible for a node.
 * @param irn The node to get the responsible isa for.
 * @return The irn operations given by the responsible isa.
 */
static inline const arch_irn_ops_t *get_irn_ops(const ir_node *irn)
{
	const ir_op          *ops;
	const arch_irn_ops_t *be_ops;

	if (is_Proj(irn)) {
		irn = get_Proj_pred(irn);
		assert(!is_Proj(irn));
	}

	ops    = get_irn_op(irn);
	be_ops = get_op_ops(ops)->be_ops;

	return be_ops;
}

const arch_register_req_t *arch_get_register_req(const ir_node *irn, int pos)
{
	const arch_irn_ops_t *ops;

	if (is_Proj(irn)) {
		assert(pos == -1);
		pos = -1-get_Proj_proj(irn);
		irn = get_Proj_pred(irn);
	}
	ops = get_irn_ops_simple(irn);
	if (pos < 0) {
		return ops->get_irn_reg_req_out(irn, -pos-1);
	} else {
		return ops->get_irn_reg_req_in(irn, pos);
	}
}

void arch_set_frame_offset(ir_node *irn, int offset)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);
	ops->set_frame_offset(irn, offset);
}

ir_entity *arch_get_frame_entity(const ir_node *irn)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);
	return ops->get_frame_entity(irn);
}

void arch_set_frame_entity(ir_node *irn, ir_entity *ent)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);
	ops->set_frame_entity(irn, ent);
}

int arch_get_sp_bias(ir_node *irn)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);
	return ops->get_sp_bias(irn);
}

arch_inverse_t *arch_get_inverse(const ir_node *irn, int i, arch_inverse_t *inverse, struct obstack *obstack)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);

	if(ops->get_inverse) {
		return ops->get_inverse(irn, i, inverse, obstack);
	} else {
		return NULL;
	}
}

int arch_possible_memory_operand(const ir_node *irn, unsigned int i)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);

	if(ops->possible_memory_operand) {
		return ops->possible_memory_operand(irn, i);
	} else {
		return 0;
	}
}

void arch_perform_memory_operand(ir_node *irn, ir_node *spill, unsigned int i)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);

	if(ops->perform_memory_operand) {
		ops->perform_memory_operand(irn, spill, i);
	} else {
		return;
	}
}

int arch_get_op_estimated_cost(const ir_node *irn)
{
	const arch_irn_ops_t *ops = get_irn_ops(irn);

	if(ops->get_op_estimated_cost) {
		return ops->get_op_estimated_cost(irn);
	} else {
		return 1;
	}
}

void arch_put_non_ignore_regs(const arch_register_class_t *cls, bitset_t *bs)
{
	unsigned i;

	for(i = 0; i < cls->n_regs; ++i) {
		if(!arch_register_type_is(&cls->regs[i], ignore))
			bitset_set(bs, i);
	}
}

int arch_reg_is_allocatable(const ir_node *irn, int pos,
                            const arch_register_t *reg)
{
	const arch_register_req_t *req = arch_get_register_req(irn, pos);

	if(req->type == arch_register_req_type_none)
		return 0;

	if(arch_register_req_is(req, limited)) {
		if (arch_register_get_class(reg) != req->cls)
			return 0;
		return rbitset_is_set(req->limited, arch_register_get_index(reg));
	}

	return req->cls == reg->reg_class;
}

const arch_register_class_t *arch_get_irn_reg_class(const ir_node *irn, int pos)
{
	const arch_register_req_t *req = arch_get_register_req(irn, pos);

	assert(req->type != arch_register_req_type_none || req->cls == NULL);

	return req->cls;
}

static inline reg_out_info_t *get_out_info(const ir_node *node)
{
	int                   pos  = 0;
	const backend_info_t *info;

	assert(get_irn_mode(node) != mode_T);
	if (is_Proj(node)) {
		pos  = get_Proj_proj(node);
		node = get_Proj_pred(node);
	}

	info = be_get_info(node);
	assert(pos >= 0 && pos < ARR_LEN(info->out_infos));
	return &info->out_infos[pos];
}


static inline reg_out_info_t *get_out_info_n(const ir_node *node, int pos)
{
	const backend_info_t *info = be_get_info(node);
	assert(!is_Proj(node));
	assert(pos >= 0 && pos < ARR_LEN(info->out_infos));
	return &info->out_infos[pos];
}


const arch_register_t *arch_get_irn_register(const ir_node *node)
{
	const reg_out_info_t *out = get_out_info(node);
	return out->reg;
}

const arch_register_t *arch_irn_get_register(const ir_node *node, int pos)
{
	const reg_out_info_t *out = get_out_info_n(node, pos);
	return out->reg;
}

void arch_irn_set_register(ir_node *node, int pos, const arch_register_t *reg)
{
	reg_out_info_t *out = get_out_info_n(node, pos);
	out->reg            = reg;
}

void arch_set_irn_register(ir_node *node, const arch_register_t *reg)
{
	reg_out_info_t *out = get_out_info(node);
	out->reg = reg;
}

arch_irn_class_t arch_irn_classify(const ir_node *node)
{
	const arch_irn_ops_t *ops = get_irn_ops(node);
	return ops->classify(node);
}

arch_irn_flags_t arch_irn_get_flags(const ir_node *node)
{
	backend_info_t *info = be_get_info(node);
	return info->flags;
}

void arch_irn_set_flags(ir_node *node, arch_irn_flags_t flags)
{
	backend_info_t *info = be_get_info(node);
	info->flags = flags;
}

void arch_irn_add_flags(ir_node *node, arch_irn_flags_t flags)
{
	backend_info_t *info = be_get_info(node);
	info->flags |= flags;
}

void arch_dump_register_req(FILE *F, const arch_register_req_t *req,
                            const ir_node *node)
{
	if (req == NULL || req->type == arch_register_req_type_none) {
		fprintf(F, "n/a");
		return;
	}

	fprintf(F, "%s", req->cls->name);

	if(arch_register_req_is(req, limited)) {
		unsigned n_regs = req->cls->n_regs;
		unsigned i;

		fprintf(F, " limited to");
		for(i = 0; i < n_regs; ++i) {
			if(rbitset_is_set(req->limited, i)) {
				const arch_register_t *reg = &req->cls->regs[i];
				fprintf(F, " %s", reg->name);
			}
		}
	}

	if(arch_register_req_is(req, should_be_same)) {
		const unsigned other = req->other_same;
		int i;

		fprintf(F, " same as");
		for (i = 0; 1U << i <= other; ++i) {
			if (other & (1U << i)) {
				ir_fprintf(F, " %+F", get_irn_n(skip_Proj_const(node), i));
			}
		}
	}

	if (arch_register_req_is(req, must_be_different)) {
		const unsigned other = req->other_different;
		int i;

		fprintf(F, " different from");
		for (i = 0; 1U << i <= other; ++i) {
			if (other & (1U << i)) {
				ir_fprintf(F, " %+F", get_irn_n(skip_Proj_const(node), i));
			}
		}
	}

	if (arch_register_req_is(req, ignore)) {
		fprintf(F, " ignore");
	}
	if (arch_register_req_is(req, produces_sp)) {
		fprintf(F, " produces_sp");
	}
}

void arch_dump_reqs_and_registers(FILE *F, const ir_node *node)
{
	int              n_ins  = get_irn_arity(node);
	int              n_outs = arch_irn_get_n_outs(node);
	arch_irn_flags_t flags  = arch_irn_get_flags(node);
	int              i;

	for (i = 0; i < n_ins; ++i) {
		const arch_register_req_t *req = arch_get_in_register_req(node, i);
		fprintf(F, "inreq #%d = ", i);
		arch_dump_register_req(F, req, node);
		fputs("\n", F);
	}
	for (i = 0; i < n_outs; ++i) {
		const arch_register_req_t *req = arch_get_out_register_req(node, i);
		fprintf(F, "outreq #%d = ", i);
		arch_dump_register_req(F, req, node);
		fputs("\n", F);
	}
	for (i = 0; i < n_outs; ++i) {
		const arch_register_t     *reg = arch_irn_get_register(node, i);
		const arch_register_req_t *req = arch_get_out_register_req(node, i);
		if (req->cls == NULL)
			continue;
		fprintf(F, "reg #%d = %s\n", i, reg != NULL ? reg->name : "n/a");
	}

	fprintf(F, "flags =");
	if (flags == arch_irn_flags_none) {
		fprintf(F, " none");
	} else {
		if (flags & arch_irn_flags_dont_spill) {
			fprintf(F, " unspillable");
		}
		if (flags & arch_irn_flags_rematerializable) {
			fprintf(F, " remat");
		}
		if (flags & arch_irn_flags_modify_flags) {
			fprintf(F, " modify_flags");
		}
	}
	fprintf(F, " (%d)\n", flags);
}

static const arch_register_req_t no_requirement = {
	arch_register_req_type_none,
	NULL,
	NULL,
	0,
	0
};
const arch_register_req_t *arch_no_register_req = &no_requirement;
