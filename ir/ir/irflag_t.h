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
 * @brief    Flags to control optimizations, inline implementation.
 * @author   Michael Beck
 * @version  $Id$
 */
#ifndef FIRM_IR_IRFLAG_T_H
#define FIRM_IR_IRFLAG_T_H

#include "firm_config.h"

#include "irflag.h"

/**
 * libFIRM optimizations flags
 */
typedef enum {
#define E_FLAG(name, value, def)    irf_##name = (1 << value),
#define I_FLAG(name, value, def)    irf_##name = (1 << value),
#define R_FLAG(name, value)

#include "irflag_t.def"
	irf_last
#undef I_FLAG
#undef E_FLAG
#undef R_FLAG
} libfirm_opts_t;

/**
 * libFIRM running flags
 */
typedef enum {
#define E_FLAG(name, value, def)
#define I_FLAG(name, value, def)
#define R_FLAG(name, value)         ir_rf_##name = (1 << value),

#include "irflag_t.def"
	ir_rf_last
#undef I_FLAG
#undef E_FLAG
#undef R_FLAG
} libfirm_running_t;

extern optimization_state_t libFIRM_opt, libFIRM_running, libFIRM_verb;
extern firm_verification_t opt_do_node_verification;

extern int firm_verbosity_level;

/** initialises the flags */
void firm_init_flags(void);

/* generate the getter functions for external access */
#define E_FLAG(name, value, def)                    \
static INLINE int _get_opt_##name(void) {           \
  return libFIRM_opt & irf_##name;                  \
}                                                   \
static INLINE int get_opt_##name##_verbose(void) {  \
  return libFIRM_verb & irf_##name;                 \
}

/* generate the getter functions for internal access */
#define I_FLAG(name, value, def)                   \
static INLINE int get_opt_##name(void) {           \
  return libFIRM_opt & irf_##name;                 \
}                                                  \
static INLINE int get_opt_##name##_verbose(void) { \
  return libFIRM_verb & irf_##name;                \
}

/* generate getter and setter functions for running flags */
#define R_FLAG(name, value)                        \
static INLINE int is_##name##_running(void) {      \
  return libFIRM_running & ir_rf_##name;           \
}                                                  \
static INLINE void set_##name##_running(int flag) {\
  if (flag) libFIRM_running |= ir_rf_##name;       \
  else      libFIRM_running &= ~ir_rf_##name;      \
}

#include "irflag_t.def"

#undef I_FLAG
#undef E_FLAG
#undef R_FLAG

static INLINE int _get_firm_verbosity (void) {
	return firm_verbosity_level;
}

static INLINE int _get_optimize (void) {
  return get_opt_optimize();
}

static INLINE firm_verification_t
get_node_verification_mode(void) {
  return opt_do_node_verification;
}

#define get_optimize()                           _get_optimize()
#define get_opt_cse()                            _get_opt_cse()
#define get_firm_verbosity()                     _get_firm_verbosity()
#define get_opt_dyn_meth_dispatch()              _get_opt_dyn_meth_dispatch()
#define get_opt_optimize_class_casts()           _get_opt_optimize_class_casts()
#define get_opt_suppress_downcast_optimization() _get_opt_suppress_downcast_optimization()

extern void firm_init_flags(void);

#endif
