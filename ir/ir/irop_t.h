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
 * @brief    Representation of opcode of intermediate operation -- private header.
 * @author   Christian Schaefer, Goetz Lindenmaier, Michael Beck
 * @version  $Id$
 */
#ifndef FIRM_IR_IROP_T_H
#define FIRM_IR_IROP_T_H

#include "irop.h"
#include "irtypes.h"
#include "tv.h"

/**
 * Frees a newly created ir operation.
 */
void free_ir_op(ir_op *code);

/** Initialize the irop module. */
void init_op(void);

/** Free memory used by irop module. */
void finish_op(void);

/**
 * Copies simply all attributes stored in the old node to the new node.
 * Assumes both have the same opcode and sufficient size.
 *
 * @param old_node  the old node from which the attributes are read
 * @param new_node  the new node to which the attributes are written
 */
void default_copy_attr(const ir_node *old_node, ir_node *new_node);

/**
 * Returns the attribute size of nodes of this opcode.
 * @note Use not encouraged, internal feature.
 */
static inline size_t get_op_attr_size (const ir_op *op) {
	return op->attr_size;
}

/**
 * Returns non-zero if op is a control flow opcode,
 * like Start, End, Jmp, Cond, Return, Raise or Bad.
 */
static inline int is_op_cfopcode(const ir_op *op) {
	return op->flags & irop_flag_cfopcode;
}

/**
 * Returns non-zero if the operation manipulates interprocedural control flow:
 * CallBegin, EndReg, EndExcept
 */
static inline int is_ip_cfopcode(const ir_op *op) {
	return op->flags & irop_flag_ip_cfopcode;
}

/** Returns non-zero if operation is commutative */
static inline int is_op_commutative(const ir_op *op) {
	return op->flags & irop_flag_commutative;
}

/** Returns non-zero if operation is fragile */
static inline int is_op_fragile(const ir_op *op) {
	return op->flags & irop_flag_fragile;
}

/** Returns non-zero if operation is forking control flow */
static inline int is_op_forking(const ir_op *op) {
	return op->flags & irop_flag_forking;
}

/** Returns non-zero if operation is a high-level op */
static inline int is_op_highlevel(const ir_op *op) {
	return op->flags & irop_flag_highlevel;
}

/** Returns non-zero if operation is a const-like op */
static inline int is_op_constlike(const ir_op *op) {
	return op->flags & irop_flag_constlike;
}

static inline int is_op_uses_memory(const ir_op *op) {
	return op->flags & irop_flag_uses_memory;
}

/** Returns non-zero if operation must always be optimized */
static inline int is_op_always_opt(const ir_op *op) {
	return op->flags & irop_flag_always_opt;
}

/** Returns non-zero if operation is a keep-like op */
static inline int is_op_keep(const ir_op *op) {
	return op->flags & irop_flag_keep;
}

/** Returns non-zero if operation must always be placed in the start block. */
static inline int is_op_start_block_placed(const ir_op *op) {
	return op->flags & irop_flag_start_block;
}

/** Returns non-zero if operation is a machine operation */
static inline int is_op_machine(const ir_op *op) {
	return op->flags & irop_flag_machine;
}

/** Returns non-zero if operation is a machine operand */
static inline int is_op_machine_operand(const ir_op *op) {
	return op->flags & irop_flag_machine_op;
}

/** Returns non-zero if operation is CSE neutral */
static inline int is_op_cse_neutral(const ir_op *op) {
	return op->flags & irop_flag_cse_neutral;
}

/** Returns non-zero if operation is a machine user op number n */
static inline int is_op_machine_user(const ir_op *op, unsigned n) {
  return op->flags & (irop_flag_user << n);
}

static inline unsigned _get_op_code(const ir_op *op) {
  return op->code;
}

static inline ident *_get_op_ident(const ir_op *op){
  return op->name;
}

static inline op_pin_state _get_op_pinned(const ir_op *op) {
  return op->pin_state;
}

static inline void _set_generic_function_ptr(ir_op *op, op_func func) {
  op->ops.generic = func;
}

static inline op_func _get_generic_function_ptr(const ir_op *op) {
  return op->ops.generic;
}

static inline const ir_op_ops *_get_op_ops(const ir_op *op) {
  return &op->ops;
}

static inline void _set_op_tag(ir_op *op, unsigned tag) {
	op->tag = tag;
}

static inline unsigned _get_op_tag(const ir_op *op) {
	return op->tag;
}

static inline void _set_op_attr(ir_op *op, void *attr) {
	op->attr = attr;
}

static inline void *_get_op_attr(const ir_op *op) {
	return op->attr;
}

#define get_op_code(op)         _get_op_code(op)
#define get_op_ident(op)        _get_op_ident(op)
#define get_op_pinned(op)       _get_op_pinned(op)
#define get_op_ops(op)          _get_op_ops(op)
#define set_op_tag(op, tag)     _set_op_tag((op), (tag))
#define get_op_tag(op)          _get_op_tag(op)
#define set_op_attr(op, attr)   _set_op_attr((op), (attr))
#define get_op_attr(op)         _get_op_attr(op)

#endif
