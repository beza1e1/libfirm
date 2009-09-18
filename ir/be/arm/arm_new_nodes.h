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
 * @author  Oliver Richter, Tobias Gneist
 * @version $Id$
 */
#ifndef FIRM_BE_ARM_ARM_NEW_NODES_H
#define FIRM_BE_ARM_ARM_NEW_NODES_H

#include "arm_nodes_attr.h"
#include "bearch_arm_t.h"

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
 * Returns the attributes of a generic Arm node.
 */
arm_attr_t *get_arm_attr(ir_node *node);
const arm_attr_t *get_arm_attr_const(const ir_node *node);

/**
 * Returns the attributes of an ARM SymConst node.
 */
arm_SymConst_attr_t *get_arm_SymConst_attr(ir_node *node);
const arm_SymConst_attr_t *get_arm_SymConst_attr_const(const ir_node *node);

/**
 * Returns the attributes of an ARM CondJmp node.
 */
arm_CondJmp_attr_t *get_arm_CondJmp_attr(ir_node *node);
const arm_CondJmp_attr_t *get_arm_CondJmp_attr_const(const ir_node *node);

/**
 * Returns the attributes of an ARM SwitchJmp node.
 */
arm_SwitchJmp_attr_t *get_arm_SwitchJmp_attr(ir_node *node);
const arm_SwitchJmp_attr_t *get_arm_SwitchJmp_attr_const(const ir_node *node);

/**
 * Returns the argument register requirements of an arm node.
 */
const arch_register_req_t **get_arm_in_req_all(const ir_node *node);

/**
 * Returns the argument register requirements of an arm node.
 */
const arch_register_req_t *get_arm_in_req(const ir_node *node, int pos);

/**
 * Sets the IN register requirements at position pos.
 */
void set_arm_req_in(ir_node *node, const arch_register_req_t *req, int pos);

/**
 * Returns the immediate value
 */
long get_arm_imm_value(const ir_node *node);

/**
 * Sets the immediate value
 */
void set_arm_imm_value(ir_node *node, long imm_value);

/**
* Return the tarval of a fpaConst
*/
tarval *get_fpaConst_value(const ir_node *node);

/**
 * Sets the tarval of a fpaConst
 */
void set_fpaConst_value(ir_node *node, tarval *tv);

/**
 * Returns the proj num
 */
int get_arm_CondJmp_proj_num(const ir_node *node);

/**
 * Sets the proj num
 */
void set_arm_CondJmp_proj_num(ir_node *node, int proj_num);

ident *get_arm_symconst_id(const ir_node *node);
void set_arm_symconst_id(ir_node *node, ident *symconst_id);

ir_node *new_r_arm_StoreStackMInc(ir_graph *irg, ir_node *block, ir_node *mem, ir_node *sp,
							      int n_regs, ir_node **regs, ir_mode *mode);

/**
 * Returns the number of projs of a SwitchJmp.
 */
int get_arm_SwitchJmp_n_projs(const ir_node *node);

/**
 * Sets the number of projs of a SwitchJmp.
 */
void set_arm_SwitchJmp_n_projs(ir_node *node, int n_projs);

/**
 * Returns the default_proj_num.
 */
long get_arm_SwitchJmp_default_proj_num(const ir_node *node);

/**
 * Sets the default_proj_num.
 */
void set_arm_SwitchJmp_default_proj_num(ir_node *node, long default_proj_num);

/**
 * Gets the shift modifier attribute.
 */
arm_shift_modifier get_arm_shift_modifier(const ir_node *node);

/* Include the generated headers */
#include "gen_arm_new_nodes.h"

#endif
