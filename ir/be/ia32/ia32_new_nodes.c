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
 * @brief       Handling of ia32 specific firm opcodes.
 * @author      Christian Wuerdig
 * @version     $Id$
 *
 * This file implements the creation of the achitecture specific firm opcodes
 * and the corresponding node constructors for the ia32 assembler irg.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "irprog_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "ircons_t.h"
#include "iropt_t.h"
#include "irop.h"
#include "firm_common_t.h"
#include "irvrfy_t.h"
#include "irprintf.h"
#include "iredges.h"
#include "error.h"
#include "raw_bitset.h"
#include "xmalloc.h"

#include "../bearch_t.h"

#include "bearch_ia32_t.h"
#include "ia32_nodes_attr.h"
#include "ia32_new_nodes.h"
#include "gen_ia32_regalloc_if.h"
#include "gen_ia32_machine.h"

/***********************************************************************************
 *      _                                   _       _             __
 *     | |                                 (_)     | |           / _|
 *   __| |_   _ _ __ ___  _ __   ___ _ __   _ _ __ | |_ ___ _ __| |_ __ _  ___ ___
 *  / _` | | | | '_ ` _ \| '_ \ / _ \ '__| | | '_ \| __/ _ \ '__|  _/ _` |/ __/ _ \
 * | (_| | |_| | | | | | | |_) |  __/ |    | | | | | ||  __/ |  | || (_| | (_|  __/
 *  \__,_|\__,_|_| |_| |_| .__/ \___|_|    |_|_| |_|\__\___|_|  |_| \__,_|\___\___|
 *                       | |
 *                       |_|
 ***********************************************************************************/

/**
 * Dumps the register requirements for either in or out.
 */
static void dump_reg_req(FILE *F, ir_node *n, const arch_register_req_t **reqs,
                         int inout) {
	char *dir = inout ? "out" : "in";
	int   max = inout ? get_ia32_n_res(n) : get_irn_arity(n);
	char  buf[1024];
	int   i;

	memset(buf, 0, sizeof(buf));

	if (reqs) {
		for (i = 0; i < max; i++) {
			fprintf(F, "%sreq #%d =", dir, i);

			if (reqs[i]->type == arch_register_req_type_none) {
				fprintf(F, " n/a");
			}

			if (reqs[i]->type & arch_register_req_type_normal) {
				fprintf(F, " %s", reqs[i]->cls->name);
			}

			if (reqs[i]->type & arch_register_req_type_limited) {
				fprintf(F, " %s",
				        arch_register_req_format(buf, sizeof(buf), reqs[i], n));
			}

			if (reqs[i]->type & arch_register_req_type_should_be_same) {
				unsigned other = reqs[i]->other_same;
				int i;

				ir_fprintf(F, " same as");
				for (i = 0; 1U << i <= other; ++i) {
					if (other & (1U << i)) {
						ir_fprintf(F, " %+F", get_irn_n(n, i));
					}
				}
			}

			if (reqs[i]->type & arch_register_req_type_should_be_different) {
				unsigned other = reqs[i]->other_different;
				int i;

				ir_fprintf(F, " different from");
				for (i = 0; 1U << i <= other; ++i) {
					if (other & (1U << i)) {
						ir_fprintf(F, " %+F", get_irn_n(n, i));
					}
				}
			}

			fprintf(F, "\n");
		}

		fprintf(F, "\n");
	}
	else {
		fprintf(F, "%sreq = N/A\n", dir);
	}
}

/**
 * Dumper interface for dumping ia32 nodes in vcg.
 * @param n        the node to dump
 * @param F        the output file
 * @param reason   indicates which kind of information should be dumped
 * @return 0 on success or != 0 on failure
 */
static int ia32_dump_node(ir_node *n, FILE *F, dump_reason_t reason) {
	ir_mode     *mode = NULL;
	int          bad  = 0;
	int          i, n_res, flags;
	const arch_register_req_t **reqs;
	const arch_register_t     **slots;

	switch (reason) {
		case dump_node_opcode_txt:
			fprintf(F, "%s", get_irn_opname(n));

			if(is_ia32_Immediate(n) || is_ia32_Const(n)) {
				const ia32_immediate_attr_t *attr
					= get_ia32_immediate_attr_const(n);

				fputc(' ', F);
				if(attr->symconst) {
					if(attr->sc_sign) {
						fputc('-', F);
					}
					fputs(get_entity_name(attr->symconst), F);
				}
				if(attr->offset != 0 || attr->symconst == NULL) {
					if(attr->offset > 0 && attr->symconst != NULL) {
						fputc('+', F);
					}
					fprintf(F, "%ld", attr->offset);
				}
			}
			else {
				const ia32_attr_t *attr = get_ia32_attr_const(n);

				if(attr->am_sc != NULL || attr->am_offs != 0)
					fputs(" [", F);

				if(attr->am_sc != NULL) {
					if(attr->data.am_sc_sign) {
						fputc('-', F);
					}
					fputs(get_entity_name(attr->am_sc), F);
				}
				if(attr->am_offs != 0) {
					if(attr->am_offs > 0 && attr->am_sc != NULL) {
						fputc('+', F);
					}
					fprintf(F, "%d", attr->am_offs);
				}

				if(attr->am_sc != NULL || attr->am_offs != 0)
					fputc(']', F);
			}
			break;

		case dump_node_mode_txt:
			if (is_ia32_Ld(n) || is_ia32_St(n)) {
				mode = get_ia32_ls_mode(n);
				fprintf(F, "[%s]", mode ? get_mode_name(mode) : "?NOMODE?");
			}
			break;

		case dump_node_nodeattr_txt:
			if (! is_ia32_Lea(n)) {
				if (is_ia32_AddrModeS(n)) {
					fprintf(F, "[AM S] ");
				} else if (is_ia32_AddrModeD(n)) {
					fprintf(F, "[AM D] ");
				}
			}

			break;

		case dump_node_info_txt:
			n_res = get_ia32_n_res(n);
			fprintf(F, "=== IA32 attr begin ===\n");

			/* dump IN requirements */
			if (get_irn_arity(n) > 0) {
				reqs = get_ia32_in_req_all(n);
				dump_reg_req(F, n, reqs, 0);
			}

			/* dump OUT requirements */
			if (n_res > 0) {
				reqs = get_ia32_out_req_all(n);
				dump_reg_req(F, n, reqs, 1);
			}

			/* dump assigned registers */
			slots = get_ia32_slots(n);
			if (slots && n_res > 0) {
				for (i = 0; i < n_res; i++) {
					const arch_register_t *reg;

					reg = slots[i];

					fprintf(F, "reg #%d = %s\n", i, reg ? arch_register_get_name(reg) : "n/a");
				}
				fprintf(F, "\n");
			}

			/* dump op type */
			fprintf(F, "op = ");
			switch (get_ia32_op_type(n)) {
				case ia32_Normal:
					fprintf(F, "Normal");
					break;
				case ia32_AddrModeD:
					fprintf(F, "AM Dest (Load+Store)");
					break;
				case ia32_AddrModeS:
					fprintf(F, "AM Source (Load)");
					break;
				default:
					fprintf(F, "unknown (%d)", get_ia32_op_type(n));
					break;
			}
			fprintf(F, "\n");

			/* dump supported am */
			fprintf(F, "AM support = ");
			switch (get_ia32_am_support(n)) {
				case ia32_am_None:
					fprintf(F, "none");
					break;
				case ia32_am_Source:
					fprintf(F, "source only (Load)");
					break;
				default:
					fprintf(F, "unknown (%d)", get_ia32_am_support(n));
					break;
			}
			fprintf(F, "\n");

			/* dump AM offset */
			if(get_ia32_am_offs_int(n) != 0) {
				fprintf(F, "AM offset = %d\n", get_ia32_am_offs_int(n));
			}

			/* dump AM symconst */
			if(get_ia32_am_sc(n) != NULL) {
				ir_entity *ent = get_ia32_am_sc(n);
				ident *id = get_entity_ld_ident(ent);
				fprintf(F, "AM symconst = %s\n", get_id_str(id));
			}

			/* dump AM scale */
			fprintf(F, "AM scale = %d\n", get_ia32_am_scale(n));

			/* dump pn code */
			if (is_ia32_SwitchJmp(n)) {
				fprintf(F, "pn_code = %ld\n", get_ia32_condcode(n));
			} else if (is_ia32_CMov(n) || is_ia32_Set(n) || is_ia32_Jcc(n)) {
				long pnc = get_ia32_condcode(n);
				fprintf(F, "pn_code = 0x%X (%s)\n", pnc, get_pnc_string(pnc & pn_Cmp_True));
			}
			else if (is_ia32_CopyB(n) || is_ia32_CopyB_i(n)) {
				fprintf(F, "size = %u\n", get_ia32_copyb_size(n));
			}

			/* dump n_res */
			fprintf(F, "n_res = %d\n", get_ia32_n_res(n));

			/* dump use_frame */
			fprintf(F, "use_frame = %d\n", is_ia32_use_frame(n));

			/* commutative */
			fprintf(F, "commutative = %d\n", is_ia32_commutative(n));

			/* need stackent */
			fprintf(F, "need stackent = %d\n", is_ia32_need_stackent(n));

			/* dump latency */
			fprintf(F, "latency = %d\n", get_ia32_latency(n));

			/* dump flags */
			fprintf(F, "flags =");
			flags = get_ia32_flags(n);
			if (flags == arch_irn_flags_none) {
				fprintf(F, " none");
			}
			else {
				if (flags & arch_irn_flags_dont_spill) {
					fprintf(F, " unspillable");
				}
				if (flags & arch_irn_flags_rematerializable) {
					fprintf(F, " remat");
				}
				if (flags & arch_irn_flags_ignore) {
					fprintf(F, " ignore");
				}
				if (flags & arch_irn_flags_modify_sp) {
					fprintf(F, " modify_sp");
				}
				if (flags & arch_irn_flags_modify_flags) {
					fprintf(F, " modify_flags");
				}
			}
			fprintf(F, " (%d)\n", flags);

			/* dump frame entity */
			fprintf(F, "frame entity = ");
			if (get_ia32_frame_ent(n)) {
				ir_fprintf(F, "%+F", get_ia32_frame_ent(n));
			}
			else {
				fprintf(F, "n/a");
			}
			fprintf(F, "\n");

			/* dump modes */
			fprintf(F, "ls_mode = ");
			if (get_ia32_ls_mode(n)) {
				ir_fprintf(F, "%+F", get_ia32_ls_mode(n));
			}
			else {
				fprintf(F, "n/a");
			}
			fprintf(F, "\n");

#ifndef NDEBUG
			/* dump original ir node name */
			fprintf(F, "orig node = ");
			if (get_ia32_orig_node(n)) {
				fprintf(F, "%s", get_ia32_orig_node(n));
			}
			else {
				fprintf(F, "n/a");
			}
			fprintf(F, "\n");
#endif /* NDEBUG */

			fprintf(F, "=== IA32 attr end ===\n");
			/* end of: case dump_node_info_txt */
			break;
	}

	return bad;
}



/***************************************************************************************************
 *        _   _                   _       __        _                    _   _               _
 *       | | | |                 | |     / /       | |                  | | | |             | |
 *   __ _| |_| |_ _ __   ___  ___| |_   / /_ _  ___| |_   _ __ ___   ___| |_| |__   ___   __| |___
 *  / _` | __| __| '__| / __|/ _ \ __| / / _` |/ _ \ __| | '_ ` _ \ / _ \ __| '_ \ / _ \ / _` / __|
 * | (_| | |_| |_| |    \__ \  __/ |_ / / (_| |  __/ |_  | | | | | |  __/ |_| | | | (_) | (_| \__ \
 *  \__,_|\__|\__|_|    |___/\___|\__/_/ \__, |\___|\__| |_| |_| |_|\___|\__|_| |_|\___/ \__,_|___/
 *                                        __/ |
 *                                       |___/
 ***************************************************************************************************/

ia32_attr_t *get_ia32_attr(ir_node *node) {
	assert(is_ia32_irn(node) && "need ia32 node to get ia32 attributes");
	return (ia32_attr_t *)get_irn_generic_attr(node);
}

const ia32_attr_t *get_ia32_attr_const(const ir_node *node) {
	assert(is_ia32_irn(node) && "need ia32 node to get ia32 attributes");
	return (const ia32_attr_t*) get_irn_generic_attr_const(node);
}

ia32_x87_attr_t *get_ia32_x87_attr(ir_node *node) {
	ia32_attr_t     *attr     = get_ia32_attr(node);
	ia32_x87_attr_t *x87_attr = CAST_IA32_ATTR(ia32_x87_attr_t, attr);
	return x87_attr;
}

const ia32_x87_attr_t *get_ia32_x87_attr_const(const ir_node *node) {
	const ia32_attr_t     *attr     = get_ia32_attr_const(node);
	const ia32_x87_attr_t *x87_attr = CONST_CAST_IA32_ATTR(ia32_x87_attr_t, attr);
	return x87_attr;
}

const ia32_asm_attr_t *get_ia32_asm_attr_const(const ir_node *node) {
	const ia32_attr_t     *attr     = get_ia32_attr_const(node);
	const ia32_asm_attr_t *asm_attr = CONST_CAST_IA32_ATTR(ia32_asm_attr_t, attr);

	return asm_attr;
}

ia32_immediate_attr_t *get_ia32_immediate_attr(ir_node *node) {
	ia32_attr_t           *attr      = get_ia32_attr(node);
	ia32_immediate_attr_t *imm_attr  = CAST_IA32_ATTR(ia32_immediate_attr_t, attr);

	return imm_attr;
}

const ia32_immediate_attr_t *get_ia32_immediate_attr_const(const ir_node *node)
{
	const ia32_attr_t           *attr     = get_ia32_attr_const(node);
	const ia32_immediate_attr_t *imm_attr = CONST_CAST_IA32_ATTR(ia32_immediate_attr_t, attr);

	return imm_attr;
}

ia32_condcode_attr_t *get_ia32_condcode_attr(ir_node *node) {
	ia32_attr_t          *attr    = get_ia32_attr(node);
	ia32_condcode_attr_t *cc_attr = CAST_IA32_ATTR(ia32_condcode_attr_t, attr);

	return cc_attr;
}

const ia32_condcode_attr_t *get_ia32_condcode_attr_const(const ir_node *node) {
	const ia32_attr_t          *attr    = get_ia32_attr_const(node);
	const ia32_condcode_attr_t *cc_attr = CONST_CAST_IA32_ATTR(ia32_condcode_attr_t, attr);

	return cc_attr;
}

ia32_copyb_attr_t *get_ia32_copyb_attr(ir_node *node) {
	ia32_attr_t       *attr       = get_ia32_attr(node);
	ia32_copyb_attr_t *copyb_attr = CAST_IA32_ATTR(ia32_copyb_attr_t, attr);

	return copyb_attr;
}

const ia32_copyb_attr_t *get_ia32_copyb_attr_const(const ir_node *node) {
	const ia32_attr_t       *attr       = get_ia32_attr_const(node);
	const ia32_copyb_attr_t *copyb_attr = CONST_CAST_IA32_ATTR(ia32_copyb_attr_t, attr);

	return copyb_attr;
}

/**
 * Gets the type of an ia32 node.
 */
ia32_op_type_t get_ia32_op_type(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.tp;
}

/**
 * Sets the type of an ia32 node.
 */
void set_ia32_op_type(ir_node *node, ia32_op_type_t tp) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->data.tp     = tp;
}

/**
 * Gets the supported address mode of an ia32 node
 */
ia32_am_type_t get_ia32_am_support(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.am_support;
}

ia32_am_arity_t get_ia32_am_arity(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.am_arity;
}

/**
 * Sets the supported address mode of an ia32 node
 */
void set_ia32_am_support(ir_node *node, ia32_am_type_t am_tp,
                         ia32_am_arity_t arity) {
	ia32_attr_t *attr     = get_ia32_attr(node);
	attr->data.am_support = am_tp;
	attr->data.am_arity   = arity;

	assert((am_tp == ia32_am_None && arity == ia32_am_arity_none) ||
	       (am_tp != ia32_am_None &&
	       ((arity == ia32_am_unary) || (arity == ia32_am_binary) || (arity == ia32_am_ternary))));
}

/**
 * Gets the address mode offset as int.
 */
int get_ia32_am_offs_int(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->am_offs;
}

/**
 * Sets the address mode offset from an int.
 */
void set_ia32_am_offs_int(ir_node *node, int offset) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->am_offs = offset;
}

void add_ia32_am_offs_int(ir_node *node, int offset) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->am_offs += offset;
}

/**
 * Returns the symconst entity associated to address mode.
 */
ir_entity *get_ia32_am_sc(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->am_sc;
}

/**
 * Sets the symconst entity associated to address mode.
 */
void set_ia32_am_sc(ir_node *node, ir_entity *entity) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->am_sc       = entity;
}

/**
 * Sets the sign bit for address mode symconst.
 */
void set_ia32_am_sc_sign(ir_node *node) {
	ia32_attr_t *attr     = get_ia32_attr(node);
	attr->data.am_sc_sign = 1;
}

/**
 * Clears the sign bit for address mode symconst.
 */
void clear_ia32_am_sc_sign(ir_node *node) {
	ia32_attr_t *attr     = get_ia32_attr(node);
	attr->data.am_sc_sign = 0;
}

/**
 * Returns the sign bit for address mode symconst.
 */
int is_ia32_am_sc_sign(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.am_sc_sign;
}

/**
 * Gets the addr mode const.
 */
int get_ia32_am_scale(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.am_scale;
}

/**
 * Sets the index register scale for address mode.
 */
void set_ia32_am_scale(ir_node *node, int scale) {
	ia32_attr_t *attr   = get_ia32_attr(node);
	attr->data.am_scale = scale;
}

void ia32_copy_am_attrs(ir_node *to, const ir_node *from)
{
	set_ia32_ls_mode(to, get_ia32_ls_mode(from));
	set_ia32_am_scale(to, get_ia32_am_scale(from));
	set_ia32_am_sc(to, get_ia32_am_sc(from));
	if(is_ia32_am_sc_sign(from))
		set_ia32_am_sc_sign(to);
	add_ia32_am_offs_int(to, get_ia32_am_offs_int(from));
	set_ia32_frame_ent(to, get_ia32_frame_ent(from));
	if (is_ia32_use_frame(from))
		set_ia32_use_frame(to);
}

/**
 * Sets the uses_frame flag.
 */
void set_ia32_use_frame(ir_node *node) {
	ia32_attr_t *attr    = get_ia32_attr(node);
	attr->data.use_frame = 1;
}

/**
 * Clears the uses_frame flag.
 */
void clear_ia32_use_frame(ir_node *node) {
	ia32_attr_t *attr    = get_ia32_attr(node);
	attr->data.use_frame = 0;
}

/**
 * Gets the uses_frame flag.
 */
int is_ia32_use_frame(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.use_frame;
}

/**
 * Sets node to commutative.
 */
void set_ia32_commutative(ir_node *node) {
	ia32_attr_t *attr         = get_ia32_attr(node);
	attr->data.is_commutative = 1;
}

/**
 * Sets node to non-commutative.
 */
void clear_ia32_commutative(ir_node *node) {
	ia32_attr_t *attr         = get_ia32_attr(node);
	attr->data.is_commutative = 0;
}

/**
 * Checks if node is commutative.
 */
int is_ia32_commutative(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.is_commutative;
}

void set_ia32_need_stackent(ir_node *node) {
	ia32_attr_t *attr     = get_ia32_attr(node);
	attr->data.need_stackent = 1;
}

void clear_ia32_need_stackent(ir_node *node) {
	ia32_attr_t *attr     = get_ia32_attr(node);
	attr->data.need_stackent = 0;
}

int is_ia32_need_stackent(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.need_stackent;
}

/**
 * Gets the mode of the stored/loaded value (only set for Store/Load)
 */
ir_mode *get_ia32_ls_mode(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->ls_mode;
}

/**
 * Sets the mode of the stored/loaded value (only set for Store/Load)
 */
void set_ia32_ls_mode(ir_node *node, ir_mode *mode) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->ls_mode     = mode;
}

/**
 * Gets the frame entity assigned to this node.
 */
ir_entity *get_ia32_frame_ent(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->frame_ent;
}

/**
 * Sets the frame entity for this node.
 */
void set_ia32_frame_ent(ir_node *node, ir_entity *ent) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->frame_ent   = ent;
	if(ent != NULL)
		set_ia32_use_frame(node);
	else
		clear_ia32_use_frame(node);
}


/**
 * Gets the instruction latency.
 */
unsigned get_ia32_latency(const ir_node *node) {
	const ir_op *op               = get_irn_op(node);
	const ia32_op_attr_t *op_attr = (ia32_op_attr_t*) get_op_attr(op);
	return op_attr->latency;
}

/**
 * Returns the argument register requirements of an ia32 node.
 */
const arch_register_req_t **get_ia32_in_req_all(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->in_req;
}

/**
 * Sets the argument register requirements of an ia32 node.
 */
void set_ia32_in_req_all(ir_node *node, const arch_register_req_t **reqs) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->in_req      = reqs;
}

/**
 * Returns the result register requirements of an ia32 node.
 */
const arch_register_req_t **get_ia32_out_req_all(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->out_req;
}

/**
 * Sets the result register requirements of an ia32 node.
 */
void set_ia32_out_req_all(ir_node *node, const arch_register_req_t **reqs) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->out_req     = reqs;
}

/**
 * Returns the argument register requirement at position pos of an ia32 node.
 */
const arch_register_req_t *get_ia32_in_req(const ir_node *node, int pos) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	if(attr->in_req == NULL)
		return arch_no_register_req;

	return attr->in_req[pos];
}

/**
 * Returns the result register requirement at position pos of an ia32 node.
 */
const arch_register_req_t *get_ia32_out_req(const ir_node *node, int pos) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	if(attr->out_req == NULL)
		return arch_no_register_req;

	return attr->out_req[pos];
}

/**
 * Sets the OUT register requirements at position pos.
 */
void set_ia32_req_out(ir_node *node, const arch_register_req_t *req, int pos) {
	ia32_attr_t *attr  = get_ia32_attr(node);
	attr->out_req[pos] = req;
}

/**
 * Sets the IN register requirements at position pos.
 */
void set_ia32_req_in(ir_node *node, const arch_register_req_t *req, int pos) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->in_req[pos] = req;
}

/**
 * Returns the register flag of an ia32 node.
 */
arch_irn_flags_t get_ia32_flags(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.flags;
}

/**
 * Sets the register flag of an ia32 node.
 */
void set_ia32_flags(ir_node *node, arch_irn_flags_t flags) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->data.flags  = flags;
}

void add_ia32_flags(ir_node *node, arch_irn_flags_t flags) {
	ia32_attr_t *attr  = get_ia32_attr(node);
	attr->data.flags  |= flags;
}

/**
 * Returns the result register slots of an ia32 node.
 */
const arch_register_t **get_ia32_slots(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->slots;
}

/**
 * Returns the number of results.
 */
int get_ia32_n_res(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return ARR_LEN(attr->slots);
}

/**
 * Returns the condition code of a node.
 */
long get_ia32_condcode(const ir_node *node)
{
	const ia32_condcode_attr_t *attr = get_ia32_condcode_attr_const(node);
	return attr->pn_code;
}

/**
 * Sets the condition code of a node
 */
void set_ia32_condcode(ir_node *node, long code)
{
	ia32_condcode_attr_t *attr = get_ia32_condcode_attr(node);
	attr->pn_code = code;
}

/**
 * Returns the condition code of a node.
 */
unsigned get_ia32_copyb_size(const ir_node *node)
{
	const ia32_copyb_attr_t *attr = get_ia32_copyb_attr_const(node);
	return attr->size;
}

/**
 * Sets the flags for the n'th out.
 */
void set_ia32_out_flags(ir_node *node, arch_irn_flags_t flags, int pos) {
	ia32_attr_t *attr = get_ia32_attr(node);
	assert(pos < ARR_LEN(attr->out_flags) && "Invalid OUT position.");
	attr->out_flags[pos] = flags;
}

/**
 * Gets the flags for the n'th out.
 */
arch_irn_flags_t get_ia32_out_flags(const ir_node *node, int pos) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	assert(pos < ARR_LEN(attr->out_flags) && "Invalid OUT position.");
	return attr->out_flags[pos];
}

/**
 * Get the list of available execution units.
 */
const be_execution_unit_t ***get_ia32_exec_units(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->exec_units;
}

/**
 * Get the exception label attribute.
 */
unsigned get_ia32_exc_label(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->data.has_except_label;
}

/**
 * Set the exception label attribute.
 */
void set_ia32_exc_label(ir_node *node, unsigned flag) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->data.has_except_label = flag;
}

/**
 * Return the exception label id.
 */
ir_label_t get_ia32_exc_label_id(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);

	assert(attr->data.has_except_label);
	return attr->exc_label;
}

/**
 * Assign the exception label id.
 */
void set_ia32_exc_label_id(ir_node *node, ir_label_t id) {
	ia32_attr_t *attr = get_ia32_attr(node);

	assert(attr->data.has_except_label);
	attr->exc_label = id;
}

#ifndef NDEBUG

/**
 * Returns the name of the original ir node.
 */
const char *get_ia32_orig_node(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return attr->orig_node;
}

/**
 * Sets the name of the original ir node.
 */
void set_ia32_orig_node(ir_node *node, const char *name) {
	ia32_attr_t *attr = get_ia32_attr(node);
	attr->orig_node   = name;
}

#endif /* NDEBUG */

/******************************************************************************************************
 *                      _       _         _   _           __                  _   _
 *                     (_)     | |       | | | |         / _|                | | (_)
 *  ___ _ __   ___  ___ _  __ _| |   __ _| |_| |_ _ __  | |_ _   _ _ __   ___| |_ _  ___  _ __    ___
 * / __| '_ \ / _ \/ __| |/ _` | |  / _` | __| __| '__| |  _| | | | '_ \ / __| __| |/ _ \| '_ \  / __|
 * \__ \ |_) |  __/ (__| | (_| | | | (_| | |_| |_| |    | | | |_| | | | | (__| |_| | (_) | | | | \__ \
 * |___/ .__/ \___|\___|_|\__,_|_|  \__,_|\__|\__|_|    |_|  \__,_|_| |_|\___|\__|_|\___/|_| |_| |___/
 *     | |
 *     |_|
 ******************************************************************************************************/

/**
 * Returns whether or not the node is an AddrModeS node.
 */
int is_ia32_AddrModeS(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return (attr->data.tp == ia32_AddrModeS);
}

/**
 * Returns whether or not the node is an AddrModeD node.
 */
int is_ia32_AddrModeD(const ir_node *node) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);
	return (attr->data.tp == ia32_AddrModeD);
}

/**
 * Checks if node is a Load or xLoad/vfLoad.
 */
int is_ia32_Ld(const ir_node *node) {
	int op = get_ia32_irn_opcode(node);
	return op == iro_ia32_Load ||
	       op == iro_ia32_xLoad ||
	       op == iro_ia32_vfld ||
	       op == iro_ia32_fld;
}

/**
 * Checks if node is a Store or xStore/vfStore.
 */
int is_ia32_St(const ir_node *node) {
	int op = get_ia32_irn_opcode(node);
	return op == iro_ia32_Store ||
	       op == iro_ia32_Store8Bit ||
	       op == iro_ia32_xStore ||
	       op == iro_ia32_vfst ||
	       op == iro_ia32_fst ||
	       op == iro_ia32_fstp;
}

/**
 * Returns the name of the OUT register at position pos.
 */
const char *get_ia32_out_reg_name(const ir_node *node, int pos) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);

	assert(pos < ARR_LEN(attr->slots) && "Invalid OUT position.");
	assert(attr->slots[pos]  && "No register assigned");

	return arch_register_get_name(attr->slots[pos]);
}

/**
 * Returns the index of the OUT register at position pos within its register class.
 */
int get_ia32_out_regnr(const ir_node *node, int pos) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);

	assert(pos < ARR_LEN(attr->slots) && "Invalid OUT position.");
	assert(attr->slots[pos]  && "No register assigned");

	return arch_register_get_index(attr->slots[pos]);
}

void ia32_swap_left_right(ir_node *node)
{
	ia32_attr_t *attr  = get_ia32_attr(node);
	ir_node     *left  = get_irn_n(node, n_ia32_binary_left);
	ir_node     *right = get_irn_n(node, n_ia32_binary_right);

	assert(is_ia32_commutative(node));
	attr->data.ins_permuted = !attr->data.ins_permuted;
	set_irn_n(node, n_ia32_binary_left,  right);
	set_irn_n(node, n_ia32_binary_right, left);
}

/**
 * Returns the OUT register at position pos.
 */
const arch_register_t *get_ia32_out_reg(const ir_node *node, int pos) {
	const ia32_attr_t *attr = get_ia32_attr_const(node);

	assert(pos < ARR_LEN(attr->slots) && "Invalid OUT position.");
	assert(attr->slots[pos]  && "No register assigned");

	return attr->slots[pos];
}

/**
 * Initializes the nodes attributes.
 */
void init_ia32_attributes(ir_node *node, arch_irn_flags_t flags,
                          const arch_register_req_t **in_reqs,
                          const arch_register_req_t **out_reqs,
                          const be_execution_unit_t ***execution_units,
                          int n_res)
{
	ir_graph        *irg  = get_irn_irg(node);
	struct obstack  *obst = get_irg_obstack(irg);
	ia32_attr_t     *attr = get_ia32_attr(node);

	set_ia32_flags(node, flags);
	set_ia32_in_req_all(node, in_reqs);
	set_ia32_out_req_all(node, out_reqs);

	attr->exec_units  = execution_units;
#ifndef NDEBUG
	attr->attr_type  |= IA32_ATTR_ia32_attr_t;
#endif

	attr->out_flags = NEW_ARR_D(int, obst, n_res);
	memset(attr->out_flags, 0, n_res * sizeof(attr->out_flags[0]));

	attr->slots = NEW_ARR_D(const arch_register_t*, obst, n_res);
	/* void* cast to suppress an incorrect warning on MSVC */
	memset((void*)attr->slots, 0, n_res * sizeof(attr->slots[0]));
}

void
init_ia32_x87_attributes(ir_node *res)
{
#ifndef NDEBUG
	ia32_attr_t *attr  = get_ia32_attr(res);
	attr->attr_type   |= IA32_ATTR_ia32_x87_attr_t;
#else
	(void) res;
#endif
	ia32_current_cg->do_x87_sim = 1;
}

void
init_ia32_asm_attributes(ir_node *res)
{
#ifndef NDEBUG
	ia32_attr_t *attr  = get_ia32_attr(res);
	attr->attr_type   |= IA32_ATTR_ia32_asm_attr_t;
#else
	(void) res;
#endif
}

void
init_ia32_immediate_attributes(ir_node *res, ir_entity *symconst,
                               int symconst_sign, long offset)
{
	ia32_immediate_attr_t *attr = get_irn_generic_attr(res);

#ifndef NDEBUG
	attr->attr.attr_type  |= IA32_ATTR_ia32_immediate_attr_t;
#endif
	attr->symconst = symconst;
	attr->sc_sign  = symconst_sign;
	attr->offset   = offset;
}

void
init_ia32_copyb_attributes(ir_node *res, unsigned size) {
	ia32_copyb_attr_t *attr = get_irn_generic_attr(res);

#ifndef NDEBUG
	attr->attr.attr_type  |= IA32_ATTR_ia32_copyb_attr_t;
#endif
	attr->size = size;
}

void
init_ia32_condcode_attributes(ir_node *res, long pnc) {
	ia32_condcode_attr_t *attr = get_irn_generic_attr(res);

#ifndef NDEBUG
	attr->attr.attr_type  |= IA32_ATTR_ia32_condcode_attr_t;
#endif
	attr->pn_code = pnc;
}

ir_node *get_ia32_result_proj(const ir_node *node)
{
	const ir_edge_t *edge;

	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		if(get_Proj_proj(proj) == 0) {
			return proj;
		}
	}
	return NULL;
}

/***************************************************************************************
 *                  _                            _                   _
 *                 | |                          | |                 | |
 *  _ __   ___   __| | ___    ___ ___  _ __  ___| |_ _ __ _   _  ___| |_ ___  _ __ ___
 * | '_ \ / _ \ / _` |/ _ \  / __/ _ \| '_ \/ __| __| '__| | | |/ __| __/ _ \| '__/ __|
 * | | | | (_) | (_| |  __/ | (_| (_) | | | \__ \ |_| |  | |_| | (__| || (_) | |  \__ \
 * |_| |_|\___/ \__,_|\___|  \___\___/|_| |_|___/\__|_|   \__,_|\___|\__\___/|_|  |___/
 *
 ***************************************************************************************/

/* default compare operation to compare attributes */
int ia32_compare_attr(const ia32_attr_t *a, const ia32_attr_t *b) {
	if (a->data.tp != b->data.tp)
		return 1;

	if (a->data.am_scale != b->data.am_scale
	    || a->data.am_sc_sign != b->data.am_sc_sign
	    || a->am_offs != b->am_offs
	    || a->am_sc != b->am_sc
	    || a->ls_mode != b->ls_mode)
		return 1;

	/* nodes with not yet assigned entities shouldn't be CSEd (important for
	 * unsigned int -> double conversions */
	if(a->data.use_frame && a->frame_ent == NULL)
		return 1;
	if(b->data.use_frame && b->frame_ent == NULL)
		return 1;

	if (a->data.use_frame != b->data.use_frame
	    || a->frame_ent != b->frame_ent)
		return 1;

	if (a->data.tp != b->data.tp)
		return 1;

	if (a->data.has_except_label != b->data.has_except_label)
		return 1;

	if (a->data.ins_permuted != b->data.ins_permuted
			|| a->data.cmp_unsigned != b->data.cmp_unsigned)
		return 1;

	return 0;
}

/** Compare nodes attributes for all "normal" nodes. */
static
int ia32_compare_nodes_attr(ir_node *a, ir_node *b)
{
	const ia32_attr_t* attr_a = get_ia32_attr_const(a);
	const ia32_attr_t* attr_b = get_ia32_attr_const(b);

	return ia32_compare_attr(attr_a, attr_b);
}

/** Compare node attributes for nodes with condition code. */
static
int ia32_compare_condcode_attr(ir_node *a, ir_node *b)
{
	const ia32_condcode_attr_t *attr_a;
	const ia32_condcode_attr_t *attr_b;

	if (ia32_compare_nodes_attr(a, b))
		return 1;

	attr_a = get_ia32_condcode_attr_const(a);
	attr_b = get_ia32_condcode_attr_const(b);

	if(attr_a->pn_code != attr_b->pn_code)
		return 1;

	return 0;
}

/** Compare node attributes for CopyB nodes. */
static
int ia32_compare_copyb_attr(ir_node *a, ir_node *b)
{
	const ia32_copyb_attr_t *attr_a;
	const ia32_copyb_attr_t *attr_b;

	if (ia32_compare_nodes_attr(a, b))
		return 1;

	attr_a = get_ia32_copyb_attr_const(a);
	attr_b = get_ia32_copyb_attr_const(b);

	if(attr_a->size != attr_b->size)
		return 1;

	return 0;
}


/** Compare ASM node attributes. */
static
int ia32_compare_asm_attr(ir_node *a, ir_node *b)
{
	const ia32_asm_attr_t *attr_a;
	const ia32_asm_attr_t *attr_b;

	if(ia32_compare_nodes_attr(a, b))
		return 1;

	attr_a = get_ia32_asm_attr_const(a);
	attr_b = get_ia32_asm_attr_const(b);

	if(attr_a->asm_text != attr_b->asm_text)
		return 1;

	return 0;
}

/** Compare node attributes for Immediates. */
static
int ia32_compare_immediate_attr(ir_node *a, ir_node *b)
{
	const ia32_immediate_attr_t *attr_a = get_ia32_immediate_attr_const(a);
	const ia32_immediate_attr_t *attr_b = get_ia32_immediate_attr_const(b);

	if(attr_a->symconst != attr_b->symconst	||
	   attr_a->sc_sign != attr_b->sc_sign ||
	   attr_a->offset != attr_b->offset)
		return 1;

	return 0;
}

/** Compare node attributes for x87 nodes. */
static
int ia32_compare_x87_attr(ir_node *a, ir_node *b)
{
	return ia32_compare_nodes_attr(a, b);
}


/* copies the ia32 attributes */
static void ia32_copy_attr(const ir_node *old_node, ir_node *new_node)
{
	ir_graph          *irg      = get_irn_irg(new_node);
	struct obstack    *obst     = get_irg_obstack(irg);
	const ia32_attr_t *attr_old = get_ia32_attr_const(old_node);
	ia32_attr_t       *attr_new = get_ia32_attr(new_node);

	/* copy the attributes */
	memcpy(attr_new, attr_old, get_op_attr_size(get_irn_op(old_node)));

	/* copy out flags */
	attr_new->out_flags =
		DUP_ARR_D(int, obst, attr_old->out_flags);
	/* copy register assignments */
	attr_new->slots =
		DUP_ARR_D(arch_register_t*, obst, attr_old->slots);
}

/* Include the generated constructor functions */
#include "gen_ia32_new_nodes.c.inl"
