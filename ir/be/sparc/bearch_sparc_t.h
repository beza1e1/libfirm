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
 * @brief   declarations for SPARC backend -- private header
 * @author  Hannes Rapp, Matthias Braun
 * @version $Id$
 */
#ifndef FIRM_BE_SPARC_BEARCH_SPARC_T_H
#define FIRM_BE_SPARC_BEARCH_SPARC_T_H

#include <stdbool.h>
#include "sparc_nodes_attr.h"
#include "be.h"

typedef struct sparc_transform_env_t  sparc_transform_env_t;
typedef struct sparc_isa_t            sparc_isa_t;

struct sparc_isa_t {
	arch_env_t  base;      /**< must be derived from arch_env_t */
    pmap       *constants;
};

/**
 * this is a struct to minimize the number of parameters
 * for transformation walker
 */
struct sparc_transform_env_t {
	dbg_info *dbg;      /**< The node debug info */
	ir_graph *irg;      /**< The irg, the node should be created in */
	ir_node  *block;    /**< The block, the node should belong to */
	ir_node  *irn;      /**< The irn, to be transformed */
	ir_mode  *mode;     /**< The mode of the irn */
};

/**
 * Sparc ABI requires some space which is always available at the top of
 * the stack. It contains:
 * 16*4 bytes space for spilling the register window
 * 1*4 byte   holding a pointer to space for agregate returns (the space is
 *            always reserved, regardless wether we have an agregate return
 *            or not)
 * 6*4 bytes  Space for spilling parameters 0-5. For the cases when someone
 *            takes the adress of a parameter. I guess this is also there so
 *            the implementation of va_args gets easier -> We can simply store
 *            param 0-5 in this spaces and then handle va_next by simply
 *            incrementing the stack pointer
 */
#define SPARC_MIN_STACKSIZE 92
#define SPARC_IMMEDIATE_MIN -4096
#define SPARC_IMMEDIATE_MAX  4095

static inline bool sparc_is_value_imm_encodeable(int32_t value)
{
	return SPARC_IMMEDIATE_MIN <= value && value <= SPARC_IMMEDIATE_MAX;
}

void sparc_finish(ir_graph *irg);

#endif
