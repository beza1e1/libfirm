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
 * @brief   Function prototypes for the assembler ir node constructors.
 * @author  Matthias Braun, Mehdi
 * @version $Id$
 */
#ifndef FIRM_BE_MIPS_MIPS_NEW_NODES_H
#define FIRM_BE_MIPS_MIPS_NEW_NODES_H

#include "mips_nodes_attr.h"

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

/**
 * Returns the attributes of an mips node.
 */
mips_attr_t *get_mips_attr(ir_node *node);
const mips_attr_t *get_mips_attr_const(const ir_node *node);
const mips_immediate_attr_t *get_mips_immediate_attr_const(const ir_node *node);
const mips_load_store_attr_t *get_mips_load_store_attr_const(
		const ir_node *node);

/**
 * Returns the argument register requirements of an mips node.
 */
const arch_register_req_t **get_mips_in_req_all(const ir_node *node);

/**
 * Returns the result register requirements of an mips node.
 */
const arch_register_req_t **get_mips_out_req_all(const ir_node *node);

/**
 * Returns the argument register requirements of an mips node.
 */
const arch_register_req_t *get_mips_in_req(const ir_node *node, int pos);

/**
 * Returns the result register requirements of an mips node.
 */
const arch_register_req_t *get_mips_out_req(const ir_node *node, int pos);

/**
 * Sets the OUT register requirements at position pos.
 */
void set_mips_req_out(ir_node *node, const arch_register_req_t *req, int pos);

/**
 * Sets the IN register requirements at position pos.
 */
void set_mips_req_in(ir_node *node, const arch_register_req_t *req, int pos);

/**
 * Returns the register flag of an mips node.
 */
arch_irn_flags_t get_mips_flags(const ir_node *node);

/**
 * Sets the register flag of an mips node.
 */
void set_mips_flags(ir_node *node, arch_irn_flags_t flags);

/**
 * Returns the result register slots of an mips node.
 */
const arch_register_t **get_mips_slots(const ir_node *node);

/**
 * Returns the name of the OUT register at position pos.
 */
const char *get_mips_out_reg_name(const ir_node *node, int pos);

/**
 * Returns the index of the OUT register at position pos within its register class.
 */
int get_mips_out_regnr(const ir_node *node, int pos);

/**
 * Returns the OUT register at position pos.
 */
const arch_register_t *get_mips_out_reg(const ir_node *node, int pos);

/* Include the generated headers */
#include "gen_mips_new_nodes.h"

#endif
