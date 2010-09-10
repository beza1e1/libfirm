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
 *          opcodes and the coresponding node constructors for the TEMPLATE
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

#include "TEMPLATE_nodes_attr.h"
#include "TEMPLATE_new_nodes.h"
#include "gen_TEMPLATE_regalloc_if.h"

/**
 * Dumper interface for dumping TEMPLATE nodes in vcg.
 * @param F        the output file
 * @param n        the node to dump
 * @param reason   indicates which kind of information should be dumped
 */
static void TEMPLATE_dump_node(FILE *F, ir_node *n, dump_reason_t reason)
{
	ir_mode *mode = NULL;

	switch (reason) {
	case dump_node_opcode_txt:
		fprintf(F, "%s", get_irn_opname(n));
		break;

	case dump_node_mode_txt:
		mode = get_irn_mode(n);

		if (mode) {
			fprintf(F, "[%s]", get_mode_name(mode));
		} else {
			fprintf(F, "[?NOMODE?]");
		}
		break;

	case dump_node_nodeattr_txt:

		/* TODO: dump some attributes which should show up */
		/* in node name in dump (e.g. consts or the like)  */

		break;

	case dump_node_info_txt:
		arch_dump_reqs_and_registers(F, n);
		break;
	}
}

const TEMPLATE_attr_t *get_TEMPLATE_attr_const(const ir_node *node)
{
	assert(is_TEMPLATE_irn(node) && "need TEMPLATE node to get attributes");
	return (const TEMPLATE_attr_t *)get_irn_generic_attr_const(node);
}

TEMPLATE_attr_t *get_TEMPLATE_attr(ir_node *node)
{
	assert(is_TEMPLATE_irn(node) && "need TEMPLATE node to get attributes");
	return (TEMPLATE_attr_t *)get_irn_generic_attr(node);
}

/**
 * Initializes the nodes attributes.
 */
static void init_TEMPLATE_attributes(ir_node *node, arch_irn_flags_t flags,
                                     const arch_register_req_t **in_reqs,
                                     const be_execution_unit_t ***execution_units,
                                     int n_res)
{
	ir_graph        *irg  = get_irn_irg(node);
	struct obstack  *obst = get_irg_obstack(irg);
	backend_info_t  *info;
	(void) execution_units;

	arch_irn_set_flags(node, flags);
	arch_set_in_register_reqs(node, in_reqs);

	info            = be_get_info(node);
	info->out_infos = NEW_ARR_D(reg_out_info_t, obst, n_res);
	memset(info->out_infos, 0, n_res * sizeof(info->out_infos[0]));
}

static void set_TEMPLATE_value(ir_node *node, tarval *value)
{
	TEMPLATE_attr_t *attr = get_TEMPLATE_attr(node);
	attr->value = value;
}

static int TEMPLATE_compare_attr(ir_node *a, ir_node *b)
{
	const TEMPLATE_attr_t *attr_a = get_TEMPLATE_attr_const(a);
	const TEMPLATE_attr_t *attr_b = get_TEMPLATE_attr_const(b);
	(void) attr_a;
	(void) attr_b;

	return 0;
}

static void TEMPLATE_copy_attr(ir_graph *irg, const ir_node *old_node,
                               ir_node *new_node)
{
	struct obstack *obst    = get_irg_obstack(irg);
	const void     *attr_old = get_irn_generic_attr_const(old_node);
	void           *attr_new = get_irn_generic_attr(new_node);
	backend_info_t *old_info = be_get_info(old_node);
	backend_info_t *new_info = be_get_info(new_node);

	/* copy the attributes */
	memcpy(attr_new, attr_old, get_op_attr_size(get_irn_op(old_node)));

	/* copy out flags */
	new_info->out_infos =
		DUP_ARR_D(reg_out_info_t, obst, old_info->out_infos);
	new_info->in_reqs = old_info->in_reqs;
}

/* Include the generated constructor functions */
#include "gen_TEMPLATE_new_nodes.c.inl"
