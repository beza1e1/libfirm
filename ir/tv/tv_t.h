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
 * @brief    Representation of and static computations on target machine
 *           values -- private header.
 * @date     2003
 * @author   Mathias Heil
 * @version  $Id$
 */
#ifndef FIRM_TV_TV_T_H
#define FIRM_TV_TV_T_H

#include "firm_common.h"

#include <assert.h>
#include "irmode.h"
#include "tv.h"

/** Debugging aid for the firmEvaluator: Marks for reserved tarvals values. */
enum reserved_id {
	resid_tarval_bad         = 1,  /**< tarval_bad marker. */
	resid_tarval_undefined   = 2,  /**< tarval_undefined marker. */
	resid_tarval_b_false     = 3,  /**< tarval_b_false marker. */
	resid_tarval_b_true      = 4,  /**< tarval_b_true marker. */
	resid_tarval_unreachable = 5,  /**< tarval_unreachable marker. */
	resid_tarval_reachable   = 6   /**< tarval_reachable marker. */
};

/**
 * Initialization of the tarval module.
 *
 * Call before init_mode().
 *
 * @param null_value
 *            The reference mode NULL value, typical 0l
 * @param support_quad_precision
 *            If non-zero, activate support for quad precision
 */
void init_tarval_1(long null_value, int support_quad_precision);

/**
 * Initialization of the tarval module.
 *
 * Call after init_mode().
 */
void init_tarval_2(void);

/**
 * Free all memory occupied by the tarval module.
 */
void finish_tarval(void);

/**
 * This struct represents the afore mentioned tarvals.
 *
 * A tarval struct consists of an internal representation of the
 * value and some additional fields further describing the value.
 *
 * ATTRIBUTES:
 *   - ir_mode *mode     The mode of the stored value
 *   - void *value       The internal representation
 *
 * @sa
 *   irmode.h for predefined modes
 */
struct tarval {
	firm_kind kind;         /**< must be k_tarval */
	ir_mode   *mode;        /**< the mode of the stored value */
	const void *value;      /**< the value stored in an internal way... */
	unsigned int length;    /**< the length of the stored value */
};

/* inline functions */
/*
 * Access routines for tarval fields ========================================
 */
static INLINE ir_mode *
_get_tarval_mode(const tarval *tv) {
	assert(tv);
	return tv->mode;
}

static INLINE tarval *
_get_tarval_bad(void) {
	return tarval_bad;
}

static INLINE tarval *
_get_tarval_undefined(void) {
	return tarval_undefined;
}

static INLINE tarval *
_get_tarval_b_false(void) {
	return tarval_b_false;
}

static INLINE tarval *
_get_tarval_b_true(void) {
	return tarval_b_true;
}

static INLINE tarval *
_get_tarval_reachable(void) {
	return tarval_reachable;
}

static INLINE tarval *
_get_tarval_unreachable(void) {
	return tarval_unreachable;
}

static INLINE int
_is_tarval(const void *thing) {
	return get_kind(thing) == k_tarval;
}

#define get_tarval_mode(tv)      _get_tarval_mode(tv)
#define get_tarval_bad()         _get_tarval_bad()
#define get_tarval_undefined()   _get_tarval_undefined()
#define get_tarval_b_false()     _get_tarval_b_false()
#define get_tarval_b_true()      _get_tarval_b_true()
#define get_tarval_unreachable() _get_tarval_unreachable()
#define get_tarval_reachable()   _get_tarval_reachable()
#define get_tarval_P_void()      _get_tarval_P_void()
#define is_tarval(thing)         _is_tarval(thing)

#endif /* FIRM_TV_TV_T_H */
