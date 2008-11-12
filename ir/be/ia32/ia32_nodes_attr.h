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
 * @brief       Type definitions for ia32 node attributes.
 * @author      Christian Wuerdig
 * @version     $Id$
 */
#ifndef FIRM_BE_IA32_IA32_NODES_ATTR_H
#define FIRM_BE_IA32_IA32_NODES_ATTR_H

#include "firm_types.h"
#include "../bearch_t.h"
#include "../bemachine.h"
#include "irnode_t.h"

enum {
	ia32_pn_Cmp_unsigned = 0x1000,
	ia32_pn_Cmp_float    = 0x2000,
};

typedef enum {
	ia32_Normal,
	ia32_AddrModeD,
	ia32_AddrModeS
} ia32_op_type_t;

typedef enum {
	ia32_am_none   = 0,
	ia32_am_unary  = 1,
	ia32_am_binary = 2
} ia32_am_type_t;

typedef enum {
	match_commutative       = 1 << 0, /**< inputs are commutative */
	match_am_and_immediates = 1 << 1, /**< node supports AM and immediate at
	                                       the same time */
	match_am                = 1 << 2, /**< node supports (32bit) source AM */
	match_8bit_am           = 1 << 3, /**< node supports 8bit source AM */
	match_16bit_am          = 1 << 4, /**< node supports 16bit source AM */
	match_immediate         = 1 << 5, /**< node supports immediates */
	match_mode_neutral      = 1 << 6, /**< 16 and 8 bit modes can be emulated
	                                       by 32 bit operations */
	match_try_am            = 1 << 7, /**< only try to produce AM node, don't
	                                       do anything if AM isn't possible */
	match_two_users         = 1 << 8  /**< the instruction uses a load two times ... */
} match_flags_t;

typedef struct ia32_op_attr_t ia32_op_attr_t;
struct ia32_op_attr_t {
	match_flags_t  flags;
	unsigned       latency;
};

#ifndef NDEBUG
typedef enum {
	IA32_ATTR_INVALID               = 0,
	IA32_ATTR_ia32_attr_t           = 1 << 0,
	IA32_ATTR_ia32_x87_attr_t       = 1 << 1,
	IA32_ATTR_ia32_asm_attr_t       = 1 << 2,
	IA32_ATTR_ia32_immediate_attr_t = 1 << 3,
	IA32_ATTR_ia32_condcode_attr_t  = 1 << 4,
	IA32_ATTR_ia32_copyb_attr_t     = 1 << 5,
	IA32_ATTR_ia32_call_attr_t      = 1 << 6
} ia32_attr_type_t;
#endif

/**
 * The generic ia32 attributes. Every node has them.
 */
typedef struct ia32_attr_t ia32_attr_t;
struct ia32_attr_t {
	except_attr  exc;               /**< the exception attribute. MUST be the first one. */
	struct ia32_attr_data_bitfield {
		unsigned tp:3;                  /**< ia32 node type. */
		unsigned am_arity:2;            /**< Indicates the address mode type supported by this node. */
		unsigned am_scale:2;            /**< The address mode scale for index register. */
		unsigned am_sc_sign:1;          /**< The sign bit of the address mode symconst. */

		unsigned use_frame:1;           /**< Indicates whether the operation uses the frame pointer or not. */
		unsigned has_except_label:1;        /**< Set if this node needs a label because of possible exception. */

		unsigned is_commutative:1;      /**< Indicates whether op is commutative or not. */

		unsigned need_stackent:1;       /**< Set to 1 if node need space on stack. */
		unsigned need_64bit_stackent:1; /**< needs a 64bit stack entity (see double->unsigned int conv) */
		unsigned need_32bit_stackent:1; /**< needs a 32bit stack entity */
		unsigned ins_permuted : 1;      /**< inputs of node have been permuted
		                                     (for commutative nodes) */
		unsigned cmp_unsigned : 1;      /**< compare should be unsigned */
		unsigned is_reload : 1;         /**< node performs a reload */
		unsigned is_spill : 1;
		unsigned is_remat : 1;
	} data;

	int        am_offs;       /**< offsets for AddrMode */
	ir_entity *am_sc;         /**< SymConst for AddrMode */

	ir_mode   *ls_mode;       /**< Load/Store mode: This is the mode of the
	                               value that is manipulated by this node. */

	ir_entity *frame_ent; /**< the frame entity attached to this node */

	const be_execution_unit_t ***exec_units; /**< list of units this operation can be executed on */

	const arch_register_req_t **in_req;  /**< register requirements for arguments */
	const arch_register_req_t **out_req; /**< register requirements for results */

	ir_label_t        exc_label;       /**< the exception label iff this instruction can throw an exception */

#ifndef NDEBUG
	const char       *orig_node;      /**< holds the name of the original ir node */
	unsigned          attr_type;      /**< bitfield indicating the attribute type */
#endif
};
COMPILETIME_ASSERT(sizeof(struct ia32_attr_data_bitfield) <= 4, attr_bitfield);

/**
 * The attributes for a Call node.
 */
typedef struct ia32_call_attr_t ia32_call_attr_t;
struct ia32_call_attr_t {
	ia32_attr_t  attr;    /**< generic attribute */
	unsigned     pop;     /**< number of bytes that get popped by the callee */
	ir_type     *call_tp; /**< The call type, copied from the original Call node. */
};

/**
 * The attributes for nodes with condition code.
 */
typedef struct ia32_condcode_attr_t ia32_condcode_attr_t;
struct ia32_condcode_attr_t {
	ia32_attr_t  attr;      /**< generic attribute */
	long         pn_code;   /**< projnum "types" (e.g. indicate compare operators */
};

/**
 * The attributes for CopyB code.
 */
typedef struct ia32_copyb_attr_t ia32_copyb_attr_t;
struct ia32_copyb_attr_t {
	ia32_attr_t  attr;      /**< generic attribute */
	unsigned     size;      /**< size of copied block */
};

/**
 * The attributes for immediates.
 */
typedef struct ia32_immediate_attr_t ia32_immediate_attr_t;
struct ia32_immediate_attr_t {
	ia32_attr_t  attr;              /**< generic attribute */
	ir_entity   *symconst;          /**< An entity if any. */
	long         offset;            /**< An offset if any. */
	unsigned     sc_sign:1;         /**< The sign bit of the symconst. */
};

/**
 * The attributes for x87 nodes.
 */
typedef struct ia32_x87_attr_t ia32_x87_attr_t;
struct ia32_x87_attr_t {
	ia32_attr_t            attr;      /**< the generic attribute */
	const arch_register_t *x87[3];    /**< register slots for x87 register */
};

typedef struct ia32_asm_reg_t ia32_asm_reg_t;
struct ia32_asm_reg_t {
	unsigned                   use_input  : 1; /* use input or output pos */
	unsigned                   valid      : 1;
	unsigned                   memory     : 1;
	unsigned                   dummy_fill : 13;
	unsigned                   inout_pos  : 16; /* in/out pos where the
	                                               register is assigned */
	const ir_mode             *mode;
};

/**
 * The attributes for ASM nodes.
 */
typedef struct ia32_asm_attr_t ia32_asm_attr_t;
struct ia32_asm_attr_t {
	ia32_x87_attr_t       x87_attr;
	ident                *asm_text;
	const ia32_asm_reg_t *register_map;
};

/* the following union is necessary to indicate to the compiler that we might want to cast
 * the structs (we use them to simulate OO-inheritance) */
union allow_casts_attr_t_ {
	ia32_attr_t            attr;
	ia32_call_attr_t       call_attr;
	ia32_condcode_attr_t   cc_attr;
	ia32_copyb_attr_t      cpy_attr;
	ia32_x87_attr_t        x87_attr;
	ia32_asm_attr_t        asm_attr;
	ia32_immediate_attr_t  immediate_attr;
};

#ifndef NDEBUG
#define CAST_IA32_ATTR(type,ptr)        (assert( ((const ia32_attr_t*)(ptr))->attr_type & IA32_ATTR_ ## type ), (type*) (ptr))
#define CONST_CAST_IA32_ATTR(type,ptr)  (assert( ((const ia32_attr_t*)(ptr))->attr_type & IA32_ATTR_ ## type ), (const type*) (ptr))
#else
#define CAST_IA32_ATTR(type,ptr)        ((type*) (ptr))
#define CONST_CAST_IA32_ATTR(type,ptr)  ((const type*) (ptr))
#endif

#endif
