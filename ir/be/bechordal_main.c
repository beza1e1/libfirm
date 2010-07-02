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
 * @brief       Driver for the chordal register allocator.
 * @author      Sebastian Hack
 * @date        29.11.2005
 * @version     $Id$
 */
#include "config.h"

#include <stdlib.h>
#include <time.h>

#include "obst.h"
#include "pset.h"
#include "list.h"
#include "bitset.h"
#include "iterator.h"

#include "lc_opts.h"
#include "lc_opts_enum.h"

#include "ircons_t.h"
#include "irmode_t.h"
#include "irgraph_t.h"
#include "irprintf_t.h"
#include "irgwalk.h"
#include "ircons.h"
#include "irdump.h"
#include "irdom.h"
#include "ircons.h"
#include "irbitset.h"
#include "irnode.h"
#include "ircons.h"
#include "debug.h"
#include "execfreq.h"
#include "iredges_t.h"
#include "error.h"

#include "bechordal_t.h"
#include "beabi.h"
#include "beutil.h"
#include "besched.h"
#include "besched.h"
#include "belive_t.h"
#include "bearch.h"
#include "beifg.h"
#include "benode.h"
#include "bestatevent.h"
#include "bestat.h"
#include "bemodule.h"
#include "be_t.h"
#include "bera.h"
#include "beirg.h"
#include "bedump_minir.h"

#include "bespillslots.h"
#include "bespill.h"
#include "belower.h"

#include "becopystat.h"
#include "becopyopt.h"
#include "bessadestr.h"
#include "beverify.h"
#include "benode.h"

#include "bepbqpcoloring.h"

static be_ra_chordal_opts_t options = {
	BE_CH_DUMP_NONE,
	BE_CH_LOWER_PERM_SWAP,
	BE_CH_VRFY_WARN,
	"",
	""
};

typedef struct _post_spill_env_t {
	be_chordal_env_t            cenv;
	ir_graph                    *irg;
	const arch_register_class_t *cls;
	double                      pre_spill_cost;
} post_spill_env_t;

static const lc_opt_enum_int_items_t lower_perm_items[] = {
	{ "copy", BE_CH_LOWER_PERM_COPY },
	{ "swap", BE_CH_LOWER_PERM_SWAP },
	{ NULL, 0 }
};

static const lc_opt_enum_int_items_t lower_perm_stat_items[] = {
	{ NULL, 0 }
};

static const lc_opt_enum_int_items_t dump_items[] = {
	{ "none",       BE_CH_DUMP_NONE       },
	{ "spill",      BE_CH_DUMP_SPILL      },
	{ "live",       BE_CH_DUMP_LIVE       },
	{ "color",      BE_CH_DUMP_COLOR      },
	{ "copymin",    BE_CH_DUMP_COPYMIN    },
	{ "ssadestr",   BE_CH_DUMP_SSADESTR   },
	{ "tree",       BE_CH_DUMP_TREE_INTV  },
	{ "constr",     BE_CH_DUMP_CONSTR     },
	{ "lower",      BE_CH_DUMP_LOWER      },
	{ "spillslots", BE_CH_DUMP_SPILLSLOTS },
	{ "appel",      BE_CH_DUMP_APPEL      },
	{ "all",        BE_CH_DUMP_ALL        },
	{ NULL, 0 }
};

static const lc_opt_enum_int_items_t be_ch_vrfy_items[] = {
	{ "off",    BE_CH_VRFY_OFF    },
	{ "warn",   BE_CH_VRFY_WARN   },
	{ "assert", BE_CH_VRFY_ASSERT },
	{ NULL, 0 }
};

static lc_opt_enum_int_var_t lower_perm_var = {
	&options.lower_perm_opt, lower_perm_items
};

static lc_opt_enum_int_var_t dump_var = {
	&options.dump_flags, dump_items
};

static lc_opt_enum_int_var_t be_ch_vrfy_var = {
	&options.vrfy_option, be_ch_vrfy_items
};

static char minir_file[256] = "";

static const lc_opt_table_entry_t be_chordal_options[] = {
	LC_OPT_ENT_ENUM_PTR ("perm",          "perm lowering options", &lower_perm_var),
	LC_OPT_ENT_ENUM_MASK("dump",          "select dump phases", &dump_var),
	LC_OPT_ENT_ENUM_PTR ("verify",        "verify options", &be_ch_vrfy_var),
	LC_OPT_ENT_STR      ("minirout",      "dump MinIR to file", minir_file, sizeof(minir_file)),
	LC_OPT_LAST
};

static be_module_list_entry_t *colorings = NULL;
static const be_ra_chordal_coloring_t *selected_coloring = NULL;

void be_register_chordal_coloring(const char *name, be_ra_chordal_coloring_t *coloring)
{
	if (selected_coloring == NULL)
		selected_coloring = coloring;

	be_add_module_to_list(&colorings, name, coloring);
}

static void be_ra_chordal_coloring(be_chordal_env_t *env)
{
	assert(selected_coloring != NULL);
	if (selected_coloring != NULL) {
		selected_coloring->allocate(env);
	}
}

static void dump(unsigned mask, ir_graph *irg,
				 const arch_register_class_t *cls,
				 const char *suffix)
{
	if ((options.dump_flags & mask) == mask) {
		if (cls) {
			char buf[256];
			snprintf(buf, sizeof(buf), "%s-%s", cls->name, suffix);
			dump_ir_graph(irg, buf);
		} else {
			dump_ir_graph(irg, suffix);
		}
	}
}

/**
 * Checks for every reload if its user can perform the load on itself.
 */
static void memory_operand_walker(ir_node *irn, void *env)
{
	const ir_edge_t  *edge, *ne;
	ir_node          *block;
	ir_node          *spill;

	(void)env;

	if (! be_is_Reload(irn))
		return;

	/* only use memory operands, if the reload is only used by 1 node */
	if (get_irn_n_edges(irn) > 1)
		return;

	spill = be_get_Reload_mem(irn);
	block = get_nodes_block(irn);

	foreach_out_edge_safe(irn, edge, ne) {
		ir_node *src = get_edge_src_irn(edge);
		int     pos  = get_edge_src_pos(edge);

		assert(src && "outedges broken!");

		if (get_nodes_block(src) == block && arch_possible_memory_operand(src, pos)) {
			arch_perform_memory_operand(src, spill, pos);
		}
	}

	/* kill the Reload */
	if (get_irn_n_edges(irn) == 0) {
		sched_remove(irn);
		set_irn_n(irn, be_pos_Reload_mem, new_Bad());
		set_irn_n(irn, be_pos_Reload_frame, new_Bad());
	}
}

/**
 * Starts a walk for memory operands if supported by the backend.
 */
void check_for_memory_operands(ir_graph *irg)
{
	irg_walk_graph(irg, NULL, memory_operand_walker, NULL);
}


static be_node_stats_t last_node_stats;

/**
 * Perform things which need to be done per register class before spilling.
 */
static void pre_spill(post_spill_env_t *pse, const arch_register_class_t *cls)
{
	be_chordal_env_t *chordal_env = &pse->cenv;
	ir_graph         *irg         = pse->irg;
	ir_exec_freq     *exec_freq   = be_get_irg_exec_freq(irg);

	pse->cls                   = cls;
	chordal_env->cls           = cls;
	chordal_env->border_heads  = pmap_create();
	chordal_env->ignore_colors = bitset_malloc(chordal_env->cls->n_regs);

	be_assure_liveness(irg);
	be_liveness_assure_chk(be_get_irg_liveness(irg));

	stat_ev_do(pse->pre_spill_cost = be_estimate_irg_costs(irg, exec_freq));

	/* put all ignore registers into the ignore register set. */
	be_put_ignore_regs(irg, pse->cls, chordal_env->ignore_colors);

	be_timer_push(T_RA_CONSTR);
	be_pre_spill_prepare_constr(irg, chordal_env->cls);
	be_timer_pop(T_RA_CONSTR);

	dump(BE_CH_DUMP_CONSTR, irg, pse->cls, "constr-pre");
}

/**
 * Perform things which need to be done per register class after spilling.
 */
static void post_spill(post_spill_env_t *pse, int iteration)
{
	be_chordal_env_t    *chordal_env = &pse->cenv;
	ir_graph            *irg         = pse->irg;
	ir_exec_freq        *exec_freq   = be_get_irg_exec_freq(irg);
	int                  colors_n    = arch_register_class_n_regs(chordal_env->cls);
	int             allocatable_regs
		= colors_n - be_put_ignore_regs(irg, chordal_env->cls, NULL);

	/* some special classes contain only ignore regs, no work to be done */
	if (allocatable_regs > 0) {
		stat_ev_dbl("bechordal_spillcosts", be_estimate_irg_costs(irg, exec_freq) - pse->pre_spill_cost);

		/*
			If we have a backend provided spiller, post spill is
			called in a loop after spilling for each register class.
			But we only need to fix stack nodes once in this case.
		*/
		be_timer_push(T_RA_SPILL_APPLY);
		check_for_memory_operands(irg);
		if (iteration == 0) {
			be_abi_fix_stack_nodes(be_get_irg_abi(irg));
		}
		be_timer_pop(T_RA_SPILL_APPLY);


		/* verify schedule and register pressure */
		be_timer_push(T_VERIFY);
		if (chordal_env->opts->vrfy_option == BE_CH_VRFY_WARN) {
			be_verify_schedule(irg);
			be_verify_register_pressure(irg, pse->cls);
		} else if (chordal_env->opts->vrfy_option == BE_CH_VRFY_ASSERT) {
			assert(be_verify_schedule(irg) && "Schedule verification failed");
			assert(be_verify_register_pressure(irg, pse->cls)
				&& "Register pressure verification failed");
		}
		be_timer_pop(T_VERIFY);

		/* Color the graph. */
		be_timer_push(T_RA_COLOR);
		be_ra_chordal_coloring(chordal_env);
		be_timer_pop(T_RA_COLOR);

		dump(BE_CH_DUMP_CONSTR, irg, pse->cls, "color");

		/* Create the ifg with the selected flavor */
		be_timer_push(T_RA_IFG);
		chordal_env->ifg = be_create_ifg(chordal_env);
		be_timer_pop(T_RA_IFG);

		stat_ev_if {
			be_ifg_stat_t   stat;
			be_node_stats_t node_stats;

			be_ifg_stat(irg, chordal_env->ifg, &stat);
			stat_ev_dbl("bechordal_ifg_nodes", stat.n_nodes);
			stat_ev_dbl("bechordal_ifg_edges", stat.n_edges);
			stat_ev_dbl("bechordal_ifg_comps", stat.n_comps);

			be_collect_node_stats(&node_stats, irg);
			be_subtract_node_stats(&node_stats, &last_node_stats);

			stat_ev_dbl("bechordal_perms_before_coal",
					node_stats[BE_STAT_PERMS]);
			stat_ev_dbl("bechordal_copies_before_coal",
					node_stats[BE_STAT_COPIES]);
		}

		be_timer_push(T_RA_COPYMIN);
		if (minir_file[0] != '\0') {
			FILE *out;

			if (strcmp(minir_file, "-") == 0) {
				out = stdout;
			} else {
				out = fopen(minir_file, "w");
				if (out == NULL) {
					panic("Cound't open minir output '%s'", minir_file);
				}
			}

			be_export_minir(out, irg);
			if (out != stdout)
				fclose(out);
		}
		co_driver(chordal_env);
		be_timer_pop(T_RA_COPYMIN);

		dump(BE_CH_DUMP_COPYMIN, irg, pse->cls, "copymin");

		/* ssa destruction */
		be_timer_push(T_RA_SSA);
		be_ssa_destruction(chordal_env);
		be_timer_pop(T_RA_SSA);

		dump(BE_CH_DUMP_SSADESTR, irg, pse->cls, "ssadestr");

		if (chordal_env->opts->vrfy_option != BE_CH_VRFY_OFF) {
			be_timer_push(T_VERIFY);
			be_ssa_destruction_check(chordal_env);
			be_timer_pop(T_VERIFY);
		}

		/* the ifg exists only if there are allocatable regs */
		be_ifg_free(chordal_env->ifg);
	}

	/* free some always allocated data structures */
	pmap_destroy(chordal_env->border_heads);
	bitset_free(chordal_env->ignore_colors);
}

/**
 * Performs chordal register allocation for each register class on given irg.
 *
 * @param irg    the graph
 * @return Structure containing timer for the single phases or NULL if no
 *         timing requested.
 */
static void be_ra_chordal_main(ir_graph *irg)
{
	const arch_env_t *arch_env = be_get_irg_arch_env(irg);
	int               j;
	int               m;
	be_chordal_env_t  chordal_env;
	struct obstack    obst;

	be_timer_push(T_RA_OTHER);

	be_timer_push(T_RA_PROLOG);

	be_assure_liveness(irg);

	chordal_env.obst          = &obst;
	chordal_env.opts          = &options;
	chordal_env.irg           = irg;
	chordal_env.border_heads  = NULL;
	chordal_env.ifg           = NULL;
	chordal_env.ignore_colors = NULL;

	obstack_init(&obst);

	be_timer_pop(T_RA_PROLOG);

	stat_ev_if {
		be_collect_node_stats(&last_node_stats, irg);
	}

	if (! arch_code_generator_has_spiller(be_get_irg_cg(irg))) {
		/* use one of the generic spiller */

		/* Perform the following for each register class. */
		for (j = 0, m = arch_env_get_n_reg_class(arch_env); j < m; ++j) {
			post_spill_env_t pse;
			const arch_register_class_t *cls
				= arch_env_get_reg_class(arch_env, j);

			if (arch_register_class_flags(cls) & arch_register_class_flag_manual_ra)
				continue;


			stat_ev_ctx_push_str("bechordal_cls", cls->name);

			stat_ev_if {
				be_do_stat_reg_pressure(irg, cls);
			}

			memcpy(&pse.cenv, &chordal_env, sizeof(chordal_env));
			pse.irg = irg;
			pre_spill(&pse, cls);

			be_timer_push(T_RA_SPILL);
			be_do_spill(irg, cls);
			be_timer_pop(T_RA_SPILL);

			dump(BE_CH_DUMP_SPILL, irg, pse.cls, "spill");

			post_spill(&pse, 0);

			stat_ev_if {
				be_node_stats_t node_stats;

				be_collect_node_stats(&node_stats, irg);
				be_subtract_node_stats(&node_stats, &last_node_stats);
				be_emit_node_stats(&node_stats, "bechordal_");

				be_copy_node_stats(&last_node_stats, &node_stats);
				stat_ev_ctx_pop("bechordal_cls");
			}
		}
	} else {
		post_spill_env_t *pse;

		/* the backend has its own spiller */
		m = arch_env_get_n_reg_class(arch_env);

		pse = ALLOCAN(post_spill_env_t, m);

		for (j = 0; j < m; ++j) {
			memcpy(&pse[j].cenv, &chordal_env, sizeof(chordal_env));
			pse[j].irg = irg;
			pre_spill(&pse[j], pse[j].cls);
		}

		be_timer_push(T_RA_SPILL);
		arch_code_generator_spill(be_get_irg_cg(irg), be_birg_from_irg(irg));
		be_timer_pop(T_RA_SPILL);
		dump(BE_CH_DUMP_SPILL, irg, NULL, "spill");

		for (j = 0; j < m; ++j) {
			post_spill(&pse[j], j);
		}
	}

	be_timer_push(T_VERIFY);
	if (chordal_env.opts->vrfy_option == BE_CH_VRFY_WARN) {
		be_verify_register_allocation(irg);
	} else if (chordal_env.opts->vrfy_option == BE_CH_VRFY_ASSERT) {
		assert(be_verify_register_allocation(irg)
				&& "Register allocation invalid");
	}
	be_timer_pop(T_VERIFY);

	be_timer_push(T_RA_EPILOG);
	lower_nodes_after_ra(irg,
	                     options.lower_perm_opt&BE_CH_LOWER_PERM_COPY ? 1 : 0);
	dump(BE_CH_DUMP_LOWER, irg, NULL, "belower-after-ra");

	obstack_free(&obst, NULL);
	be_liveness_invalidate(be_get_irg_liveness(irg));
	be_timer_pop(T_RA_EPILOG);

	be_timer_pop(T_RA_OTHER);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_chordal_main);
void be_init_chordal_main(void)
{
	static be_ra_t be_ra_chordal_allocator = {
		be_ra_chordal_main,
	};

	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *ra_grp = lc_opt_get_grp(be_grp, "ra");
	lc_opt_entry_t *chordal_grp = lc_opt_get_grp(ra_grp, "chordal");

	be_register_allocator("chordal", &be_ra_chordal_allocator);

	lc_opt_add_table(chordal_grp, be_chordal_options);
	be_add_module_list_opt(chordal_grp, "coloring", "select coloring methode", &colorings, (void**) &selected_coloring);
}
