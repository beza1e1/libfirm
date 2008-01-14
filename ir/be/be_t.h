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

#include <libcore/lc_timing.h>

#include "firm_types.h"
#include "obst.h"
#include "debug.h"
#include "bitset.h"

#include "be.h"
#include "bearch.h"
#include "be_dbgout.h"
#include "beirg.h"

#define DUMP_NONE       0
#define DUMP_INITIAL    (1 << 0)
#define DUMP_ABI        (1 << 1)
#define DUMP_SCHED      (1 << 2)
#define DUMP_PREPARED   (1 << 3)
#define DUMP_RA         (1 << 4)
#define DUMP_FINAL      (1 << 5)
#define DUMP_BE         (1 << 6)

enum {
	BE_TIME_OFF,
	BE_TIME_ON
};

enum {
	BE_VRFY_OFF,
	BE_VRFY_WARN,
	BE_VRFY_ASSERT
};

enum {
	BE_SCHED_LIST,
	BE_SCHED_ILP
};

/** Backend options */
struct be_options_t {
	unsigned dump_flags;      /**< backend dumping flags */
	int  timing;              /**< time the backend phases */
	int  opt_profile;         /**< instrument code for profiling */
	int  omit_fp;             /**< try to omit the frame pointer */
	int  stabs_debug_support; /**< enable stabs debugging support */
	int  vrfy_option;         /**< backend verify option */
	int  scheduler;           /**< the scheduler */
	char ilp_server[128];     /**< the ilp server name */
	char ilp_solver[128];     /**< the ilp solver name */
	int  statev;              /**< enable stat event dumping */
	char printev[128];
};

struct be_main_env_t {
	struct obstack         obst;
	arch_env_t            *arch_env;
	be_options_t          *options;
	arch_code_generator_t *cg;
	arch_irn_handler_t    *phi_handler;
	dbg_handle            *db_handle;
	const char            *cup_name;
};

/**
* Put the registers to be ignored in this IRG into a bitset.
* @param birg The backend IRG data structure.
* @param cls  The register class.
* @param bs   The bitset (may be NULL).
* @return The number of registers to be ignored.
*/
unsigned be_put_ignore_regs(const be_irg_t *birg,
		const arch_register_class_t *cls, bitset_t *bs);

extern int be_timing;

#define BE_TIMER_PUSH(timer)                                              \
    if (be_timing) {                                                      \
        int res = lc_timer_push(timer);                                   \
        (void) res;                                                       \
		assert(res && "Timer already on stack, cannot be pushed twice."); \
    }

#define BE_TIMER_POP(timer)                                               \
    if (be_timing) {                                                      \
        lc_timer_t *tmp = lc_timer_pop();                                 \
        (void) tmp;                                                       \
        assert(tmp == timer && "Attempt to pop wrong timer.");            \
    }

extern lc_timer_t *t_abi;
extern lc_timer_t *t_codegen;
extern lc_timer_t *t_sched;
extern lc_timer_t *t_constr;
extern lc_timer_t *t_finish;
extern lc_timer_t *t_emit;
extern lc_timer_t *t_other;
extern lc_timer_t *t_execfreq;
extern lc_timer_t *t_verify;
extern lc_timer_t *t_heights;
extern lc_timer_t *t_live;         /**< timer for liveness calculation */
extern lc_timer_t *t_ssa_constr;   /**< timer for ssa reconstruction */
extern lc_timer_t *t_ra_prolog;    /**< timer for prolog */
extern lc_timer_t *t_ra_epilog;    /**< timer for epilog */
extern lc_timer_t *t_ra_constr;    /**< timer for spill constraints */
extern lc_timer_t *t_ra_spill;     /**< timer for spilling */
extern lc_timer_t *t_ra_spill_apply;
extern lc_timer_t *t_ra_color;     /**< timer for graph coloring */
extern lc_timer_t *t_ra_ifg;       /**< timer for building interference graph */
extern lc_timer_t *t_ra_copymin;   /**< timer for copy minimization */
extern lc_timer_t *t_ra_ssa;       /**< timer for ssa destruction */
extern lc_timer_t *t_ra_other;     /**< timer for remaining stuff */

#endif /* FIRM_BE_BE_T_H */
