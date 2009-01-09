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
 * @brief   declarations for ARM node attributes
 * @author  Oliver Richter, Tobias Gneist, Michael Beck
 * @version $Id$
 */
#ifndef FIRM_BE_ARM_ARM_NODES_ATTR_H
#define FIRM_BE_ARM_ARM_NODES_ATTR_H

#include "firm_types.h"
#include "irnode_t.h"
#include "../bearch_t.h"

/**
 * Possible ARM register shift types.
 */
typedef enum _arm_shift_modifier {
	ARM_SHF_NONE = 0,   /**< no shift */
	ARM_SHF_IMM  = 1,   /**< immediate operand with implicit ROR */
	ARM_SHF_ASR  = 2,   /**< arithmetic shift right */
	ARM_SHF_LSL  = 3,   /**< logical shift left */
	ARM_SHF_LSR  = 4,   /**< logical shift right */
	ARM_SHF_ROR  = 5,   /**< rotate right */
	ARM_SHF_RRX  = 6,   /**< rotate right through carry bits */
} arm_shift_modifier;

/** True, if the modifier implies a shift argument */
#define ARM_HAS_SHIFT(mod)          ((mod) > ARM_SHF_IMM)

/** get the shift modifier from flags */
#define ARM_GET_SHF_MOD(attr)       ((attr)->instr_fl & 7)

/** set the shift modifier to flags */
#define ARM_SET_SHF_MOD(attr, mod)  ((attr)->instr_fl = (((attr)->instr_fl & ~7) | (mod)))

/** fpa immediate bit */
#define ARM_FPA_IMM  (1 << 3)   /**< fpa floating point immediate */

#define ARM_GET_FPA_IMM(attr)        ((attr)->instr_fl & ARM_FPA_IMM)
#define ARM_SET_FPA_IMM(attr)        ((attr)->instr_fl |= ARM_FPA_IMM)
#define ARM_CLR_FPA_IMM(attr)        ((attr)->instr_fl &= ~ARM_FPA_IMM)

/**
 * Possible ARM condition codes.
 */
typedef enum _arm_condition {
	ARM_COND_EQ = 0,   /**< Equal, Z set. */
	ARM_COND_NE = 1,   /**< Not Equal, Z clear */
	ARM_COND_CS = 2,   /**< Carry set, unsigned >=, C set */
	ARM_COND_CC = 3,   /**< Carry clear, unsigned <, C clear */
	ARM_COND_MI = 4,   /**< Minus/Negative, N set */
	ARM_COND_PL = 5,   /**< Plus/Positive or Zero, N clear */
	ARM_COND_VS = 6,   /**< Overflow, V set */
	ARM_COND_VC = 7,   /**< No overflow, V clear */
	ARM_COND_HI = 8,   /**< unsigned >, C set and Z clear */
	ARM_COND_LS = 9,   /**< unsigned <=, C clear or Z set */
	ARM_COND_GE = 10,  /**< signed >=, N == V */
	ARM_COND_LT = 11,  /**< signed <, N != V */
	ARM_COND_GT = 12,  /**< signed >, Z clear and N == V */
	ARM_COND_LE = 13,  /**< signed <=, Z set or N != V */
	ARM_COND_AL = 14,  /**< Always (unconditional) */
	ARM_COND_NV = 15   /**< forbidden */
} arm_condition;

/** Get the condition code from flags */
#define ARM_GET_COND(attr)        (((attr)->instr_fl >> 4) & 15)

/** Set the condition code to flags */
#define ARM_SET_COND(attr, code)  ((attr)->instr_fl = (((attr)->instr_fl & ~(15 << 4)) | ((code) << 4)))

/** Encoding for fpa immediates */
enum fpa_immediates {
	fpa_null = 0,
	fpa_one,
	fpa_two,
	fpa_three,
	fpa_four,
	fpa_five,
	fpa_ten,
	fpa_half,
	fpa_max
};

/** Generic ARM node attributes. */
typedef struct _arm_attr_t {
	except_attr      exc;                /**< the exception attribute. MUST be the first one. */

	const arch_register_req_t **in_req;  /**< register requirements for arguments */
	const arch_register_req_t **out_req; /**< register requirements for results */

	ir_mode  *op_mode;                   /**< operation mode if different from node's mode */
	unsigned instr_fl;                   /**< condition code, shift modifier */
	long     imm_value;                  /**< immediate */
} arm_attr_t;

/** Attributes for a SymConst */
typedef struct _arm_SymConst_attr_t {
	arm_attr_t  attr;                   /**< base attributes */
	ident       *symconst_id;           /**< for SymConsts: its ident */
} arm_SymConst_attr_t;

/** Attributes for a CondJmp */
typedef struct _arm_CondJmp_attr_t {
	arm_attr_t  attr;                   /**< base attributes */
	int         proj_num;
} arm_CondJmp_attr_t;

/** Attributes for a SwitchJmp */
typedef struct _arm_SwitchJmp_attr_t {
	arm_attr_t  attr;                   /**< base attributes */
	int         n_projs;
	long        default_proj_num;
} arm_SwitchJmp_attr_t;

/** Attributes for a fpaConst */
typedef struct _arm_fpaConst_attr_t {
	arm_attr_t  attr;                   /**< base attributes */
	tarval      *tv;                    /**< the tarval representing the FP const */
} arm_fpaConst_attr_t;

/**
 * Returns the shift modifier string.
 */
const char *arm_shf_mod_name(arm_shift_modifier mod);

/**
 * Return the fpa immediate from the encoding.
 */
const char *arm_get_fpa_imm_name(long imm_value);

#define CAST_ARM_ATTR(type,ptr)        ((type *)(ptr))
#define CONST_CAST_ARM_ATTR(type,ptr)  ((const type *)(ptr))

#endif
