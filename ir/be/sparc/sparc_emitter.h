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
 * @brief    declarations for emit functions
 * @version  $Id: TEMPLATE_emitter.h 26317 2009-08-05 10:53:46Z matze $
 */
#ifndef FIRM_BE_TEMPLATE_TEMPLATE_EMITTER_H
#define FIRM_BE_TEMPLATE_TEMPLATE_EMITTER_H

#include "irargs_t.h"
#include "irnode.h"
#include "debug.h"

#include "../bearch.h"
#include "../beemitter.h"

#include "bearch_sparc_t.h"

//int get_TEMPLATE_reg_nr(ir_node *irn, int posi, int in_out);
//const char *get_TEMPLATE_in_reg_name(ir_node *irn, int pos);

void sparc_emit_immediate(const ir_node *node);
void sparc_emit_mode(const ir_node *node);
void sparc_emit_source_register(const ir_node *node, int pos);
void sparc_emit_reg_or_imm(const ir_node *node, int pos);
void sparc_emit_dest_register(const ir_node *node, int pos);
void sparc_emit_offset(const ir_node *node);
void sparc_emit_load_mode(const ir_node *node);
void sparc_emit_store_mode(const ir_node *node);

void sparc_gen_routine(const sparc_code_gen_t *cg, ir_graph *irg);

void sparc_init_emitter(void);

#endif