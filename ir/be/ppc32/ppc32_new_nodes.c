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
 * @brief  This file implements the creation of the architecture specific firm
 *         opcodes and the corresponding node constructors for the ppc assembler
 *         irg.
 * @author  Moritz Kroll, Jens Mueller
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
#include "irvrfy_t.h"
#include "irprintf.h"
#include "xmalloc.h"

#include "../bearch.h"

#include "ppc32_nodes_attr.h"
#include "ppc32_new_nodes.h"
#include "gen_ppc32_regalloc_if.h"



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
 * Dumper interface for dumping ppc32 nodes in vcg.
 * @param n        the node to dump
 * @param F        the output file
 * @param reason   indicates which kind of information should be dumped
 * @return 0 on success or != 0 on failure
 */
static int ppc32_dump_node(ir_node *n, FILE *F, dump_reason_t reason)
{
  	ir_mode     *mode = NULL;
	int          bad  = 0;

	switch (reason) {
		case dump_node_opcode_txt:
			fprintf(F, "%s", get_irn_opname(n));
			break;

		case dump_node_mode_txt:
			mode = get_irn_mode(n);

			if (mode) {
				fprintf(F, "[%s]", get_mode_name(mode));
			}
			else {
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

ppc32_attr_t *get_ppc32_attr(ir_node *node) {
	assert(is_ppc32_irn(node) && "need ppc node to get attributes");
	return (ppc32_attr_t *)get_irn_generic_attr(node);
}

const ppc32_attr_t *get_ppc32_attr_const(const ir_node *node) {
	assert(is_ppc32_irn(node) && "need ppc node to get attributes");
	return (const ppc32_attr_t *)get_irn_generic_attr_const(node);
}



/**
 * Returns the argument register requirements of a ppc node.
 */
const arch_register_req_t **get_ppc32_in_req_all(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->in_req;
}

/**
 * Returns the argument register requirement at position pos of an ppc node.
 */
const arch_register_req_t *get_ppc32_in_req(const ir_node *node, int pos) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->in_req[pos];
}

/**
 * Sets the IN register requirements at position pos.
 */
void set_ppc32_req_in(ir_node *node, const arch_register_req_t *req, int pos) {
	ppc32_attr_t *attr  = get_ppc32_attr(node);
	attr->in_req[pos] = req;
}

/**
 * Sets the type of the constant (if any)
 * May be either iro_Const or iro_SymConst
 */
/* void set_ppc32_type(const ir_node *node, opcode type) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->type      = type;
}  */

/**
 * Returns the type of the content (if any)
 */
ppc32_attr_content_type get_ppc32_type(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->content_type;
}

/**
 * Sets a tarval type content (also updating the content_type)
 */
void set_ppc32_constant_tarval(ir_node *node, tarval *const_tarval) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->content_type = ppc32_ac_Const;
	attr->data.constant_tarval = const_tarval;
}

/**
 * Returns a tarval type constant
 */
tarval *get_ppc32_constant_tarval(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->data.constant_tarval;
}

/**
 * Sets an ident type constant (also updating the content_type)
 */
void set_ppc32_symconst_ident(ir_node *node, ident *symconst_ident) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->content_type = ppc32_ac_SymConst;
	attr->data.symconst_ident = symconst_ident;
}

/**
 * Returns an ident type constant
 */
ident *get_ppc32_symconst_ident(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->data.symconst_ident;
}


/**
 * Sets an entity (also updating the content_type)
 */
void set_ppc32_frame_entity(ir_node *node, ir_entity *ent) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->content_type = ppc32_ac_FrameEntity;
	attr->data.frame_entity = ent;
}

/**
 * Returns an entity
 */
ir_entity *get_ppc32_frame_entity(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->data.frame_entity;
}

/**
 * Sets a Rlwimi const (also updating the content_type)
 */
void set_ppc32_rlwimi_const(ir_node *node, unsigned shift, unsigned maskA, unsigned maskB) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->content_type = ppc32_ac_RlwimiConst;
	attr->data.rlwimi_const.shift = shift;
	attr->data.rlwimi_const.maskA = maskA;
	attr->data.rlwimi_const.maskB = maskB;
}

/**
 * Returns the rlwimi const structure
 */
const rlwimi_const_t *get_ppc32_rlwimi_const(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return &attr->data.rlwimi_const;
}

/**
 * Sets a Proj number (also updating the content_type)
 */
void set_ppc32_proj_nr(ir_node *node, int proj_nr) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->content_type = ppc32_ac_BranchProj;
	attr->data.proj_nr = proj_nr;
}

/**
 * Returns the proj number
 */
int get_ppc32_proj_nr(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->data.proj_nr;
}

/**
 * Sets an offset for a memory access (also updating the content_type)
 */
void set_ppc32_offset(ir_node *node, int offset) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->content_type = ppc32_ac_Offset;
	attr->data.offset  = offset;
}

/**
 * Returns the offset
 */
int get_ppc32_offset(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->data.offset;
}

/**
 * Sets the offset mode (ppc32_ao_None, ppc32_ao_Lo16, ppc32_ao_Hi16 or ppc32_ao_Ha16)
 */
void set_ppc32_offset_mode(ir_node *node, ppc32_attr_offset_mode mode) {
	ppc32_attr_t *attr = get_ppc32_attr(node);
	attr->offset_mode = mode;
}

/**
 * Returns the offset mode
 */
ppc32_attr_offset_mode get_ppc32_offset_mode(const ir_node *node) {
	const ppc32_attr_t *attr = get_ppc32_attr_const(node);
	return attr->offset_mode;
}


/**
 * Initializes ppc specific node attributes
 */
void init_ppc32_attributes(ir_node *node, int flags,
						 const arch_register_req_t **in_reqs,
						 const be_execution_unit_t ***execution_units,
						 int n_res) {
	ir_graph       *irg  = get_irn_irg(node);
	struct obstack *obst = get_irg_obstack(irg);
	ppc32_attr_t   *attr = get_ppc32_attr(node);
	backend_info_t  *info;
	(void) execution_units;

	arch_irn_set_flags(node, flags);
	attr->in_req  = in_reqs;

	attr->content_type = ppc32_ac_None;
	attr->offset_mode  = ppc32_ao_Illegal;
	attr->data.empty   = NULL;

	info            = be_get_info(node);
	info->out_infos = NEW_ARR_D(reg_out_info_t, obst, n_res);
	memset(info->out_infos, 0, n_res * sizeof(info->out_infos[0]));
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

/* Include the generated constructor functions */
#include "gen_ppc32_new_nodes.c.inl"
