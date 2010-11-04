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
 * @brief   Internal backend global data structures.
 * @author  Sebastian Hack
 * @version $Id$
 */
#ifndef FIRM_BE_BE_T_H
#define FIRM_BE_BE_T_H

#include "firm_types.h"
#include "obst.h"
#include "debug.h"
#include "bitset.h"
#include "timing.h"
#include "pmap.h"

#include "be.h"
#include "be_types.h"

enum {
	DUMP_NONE     = 0,
	DUMP_INITIAL  = 1 << 0,
	DUMP_ABI      = 1 << 1,
	DUMP_SCHED    = 1 << 2,
	DUMP_PREPARED = 1 << 3,
	DUMP_RA       = 1 << 4,
	DUMP_FINAL    = 1 << 5,
	DUMP_BE       = 1 << 6
};

enum {
	BE_TIME_OFF,
	BE_TIME_ON
};

enum {
	BE_VERIFY_OFF,
	BE_VERIFY_WARN,
	BE_VERIFY_ASSERT
};

/** Backend options */
struct be_options_t {
	unsigned dump_flags;      /**< backend dumping flags */
	int  timing;              /**< time the backend phases */
	int  opt_profile;         /**< instrument code for profiling */
	int  omit_fp;             /**< try to omit the frame pointer */
	int  pic;                 /**< create position independent code */
	int  gprof;               /**< create gprof compatible profiling code */
	int  verify_option;       /**< backend verify option */
	char target_os[128];      /**< target operating system name */
	char ilp_server[128];     /**< the ilp server name */
	char ilp_solver[128];     /**< the ilp solver name */
	int  statev;              /**< enable stat event dumping */
	char filtev[128];         /**< filter mask for stat events (regex is supported) */
};

struct be_main_env_t {
	arch_env_t   *arch_env;
	be_options_t *options;              /**< backend options */
	const char   *cup_name;             /**< name of the compilation unit */
	pmap         *ent_trampoline_map;   /**< A map containing PIC trampolines for methods. */
	ir_type      *pic_trampolines_type; /**< Class type containing all trampolines */
	pmap         *ent_pic_symbol_map;
	ir_type      *pic_symbols_type;
};

extern asm_constraint_flags_t asm_constraint_flags[256];

void be_init_default_asm_constraint_flags(void);

void be_put_allocatable_regs(const ir_graph *irg,
                             const arch_register_class_t *cls, bitset_t *bs);

void be_set_allocatable_regs(const ir_graph *irg,
                             const arch_register_class_t *cls,
                             unsigned *raw_bitset);

unsigned be_get_n_allocatable_regs(const ir_graph *irg,
                                   const arch_register_class_t *cls);

/**
 * Initialize the backend. Must be run first in init_firm();
 */
void firm_be_init(void);
void firm_be_finish(void);

extern int be_timing;

typedef enum {
	T_FIRST,
	T_ABI = T_FIRST,
	T_CODEGEN,
	T_RA_PREPARATION,
	T_SCHED,
	T_CONSTR,
	T_FINISH,
	T_EMIT,
	T_VERIFY,
	T_OTHER,
	T_HEIGHTS,
	T_LIVE,
	T_EXECFREQ,
	T_SSA_CONSTR,
	T_RA_PROLOG,
	T_RA_EPILOG,
	T_RA_CONSTR,
	T_RA_SPILL,
	T_RA_SPILL_APPLY,
	T_RA_COLOR,
	T_RA_IFG,
	T_RA_COPYMIN,
	T_RA_SSA,
	T_RA_OTHER,
	T_LAST = T_RA_OTHER
} be_timer_id_t;
ENUM_COUNTABLE(be_timer_id_t)
extern ir_timer_t *be_timers[T_LAST+1];

static inline void be_timer_push(be_timer_id_t id)
{
	int res;
	if (!be_timing)
		return;

	assert(id <= T_LAST);
	res = ir_timer_push(be_timers[id]);
	(void) res;
	assert(res && "Timer already on stack, cannot be pushed twice.");
}

static inline void be_timer_pop(be_timer_id_t id)
{
	ir_timer_t *tmp;
	if (!be_timing)
		return;

	tmp = ir_timer_pop();
	(void) tmp;
	(void) id;
	assert(tmp == be_timers[id] && "Attempt to pop wrong timer.");
}

#endif
