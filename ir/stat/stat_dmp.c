/*
 * Project:     libFIRM
 * File name:   ir/ir/stat_dmp.c
 * Purpose:     Statistics for Firm.
 * Author:      Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "stat_dmp.h"
#include "irhooks.h"

/**
 * names of the optimizations
 */
static const struct {
	hook_opt_kind kind;
	const char    *name;
} opt_names[] = {
	{ HOOK_OPT_DEAD_BLOCK,   "dead block elimination" },
	{ HOOK_OPT_STG,          "straightening optimization" },
	{ HOOK_OPT_IFSIM,        "if simplification" },
	{ HOOK_OPT_CONST_EVAL,   "constant evaluation" },
	{ HOOK_OPT_ALGSIM,       "algebraic simplification" },
	{ HOOK_OPT_PHI,          "Phi optmization" },
	{ HOOK_OPT_SYNC,         "Sync optmization" },
	{ HOOK_OPT_WAW,          "Write-After-Write optimization" },
	{ HOOK_OPT_WAR,          "Write-After-Read optimization" },
	{ HOOK_OPT_RAW,          "Read-After-Write optimization" },
	{ HOOK_OPT_RAR,          "Read-After-Read optimization" },
	{ HOOK_OPT_RC,           "Read-a-Const optimization" },
	{ HOOK_OPT_TUPLE,        "Tuple optimization" },
	{ HOOK_OPT_ID,           "ID optimization" },
	{ HOOK_OPT_CSE,          "Common subexpression elimination" },
	{ HOOK_OPT_STRENGTH_RED, "Strength reduction" },
	{ HOOK_OPT_ARCH_DEP,     "Architecture dependant optimization" },
	{ HOOK_OPT_REASSOC,      "Reassociation optimization" },
	{ HOOK_OPT_POLY_CALL,    "Polymorphic call optimization" },
	{ HOOK_OPT_IF_CONV,      "an if conversion was tried" },
	{ HOOK_OPT_FUNC_CALL,    "Real function call optimization" },
	{ HOOK_OPT_CONFIRM,      "Confirm-based optimization: replacement" },
	{ HOOK_OPT_CONFIRM_C,    "Confirm-based optimization: replaced by const" },
	{ HOOK_OPT_CONFIRM_E,    "Confirm-based optimization: evaluated" },
	{ HOOK_OPT_EXC_REM,      "a exception edge was removed due to a Confirmation prove" },
	{ HOOK_LOWERED,          "Lowered" },
	{ HOOK_BACKEND,          "Backend transformation" },
	{ FS_OPT_NEUTRAL_0,      "algebraic simplification: a op 0 = 0 op a = a" },
	{ FS_OPT_NEUTRAL_1,      "algebraic simplification: a op 1 = 1 op a = a" },
	{ FS_OPT_ADD_A_A,        "algebraic simplification: a + a = a * 2" },
	{ FS_OPT_ADD_A_MINUS_B,  "algebraic simplification: a + -b = a - b" },
	{ FS_OPT_ADD_SUB,        "algebraic simplification: (a + x) - x = (a - x) + x = a" },
	{ FS_OPT_ADD_MUL_A_X_A,  "algebraic simplification: a * x + a = a * (x + 1)" },
	{ FS_OPT_SUB_0_A,        "algebraic simplification: 0 - a = -a" },
	{ FS_OPT_SUB_MUL_A_X_A,  "algebraic simplification: a * x - a = a * (x - 1)" },
	{ FS_OPT_MUL_MINUS_1,    "algebraic simplification: a * -1 = -a" },
	{ FS_OPT_OR,             "algebraic simplification: a | a = a | 0 = 0 | a = a" },
	{ FS_OPT_AND,            "algebraic simplification: a & 0b1...1 = 0b1...1 & a =  a & a = a" },
	{ FS_OPT_EOR_A_A,        "algebraic simplification: a ^ a = 0" },
	{ FS_OPT_EOR_TO_NOT_BOOL,"algebraic simplification: bool ^ 1 = !bool" },
	{ FS_OPT_EOR_TO_NOT,     "algebraic simplification: x ^ 0b1..1 = ~x" },
	{ FS_OPT_NOT_CMP,        "algebraic simplification: !(a cmp b) = a !cmp b" },
	{ FS_OPT_OR_SHFT_TO_ROT, "algebraic simplification: (x << c) | (x >> (bits - c)) == Rot(x, c)" },
	{ FS_OPT_REASSOC_SHIFT,  "algebraic simplification: (x SHF c1) SHF c2 = x SHF (c1+c2)" },
	{ FS_OPT_CONV,           "algebraic simplification: Conv could be removed" },
	{ FS_OPT_CAST,           "algebraic simplification: a Cast could be removed" },
	{ FS_OPT_MIN_MAX_EQ,     "algebraic simplification: Min(a,a) = Max(a,a) = a" },
	{ FS_OPT_MUX_C,          "algebraic simplification: Mux(C, f, t) = C ? t : f" },
	{ FS_OPT_MUX_EQ,         "algebraic simplification: Mux(v, x, x) = x" },
	{ FS_OPT_MUX_TRANSFORM,  "algebraic simplification: Mux(a, b, c) = b OR Mux(a,b, c) = c" },
	{ FS_OPT_MUX_TO_MIN,     "algebraic simplification: Mux(a < b, a, b) = Min(a,b)" },
	{ FS_OPT_MUX_TO_MAX,     "algebraic simplification: Mux(a > b, a, b) = Max(a,b)" },
	{ FS_OPT_MUX_TO_ABS,     "algebraic simplification: Mux(a > b, a, b) = Abs(a,b)" },
	{ FS_OPT_MUX_TO_SHR,     "algebraic simplification: Mux(a > b, a, b) = a >> b" },
	{ FS_BE_IA32_LEA,        "ia32 Backend transformation: Lea was created" },
	{ FS_BE_IA32_LOAD_LEA,   "ia32 Backend transformation: Load merged with a Lea" },
	{ FS_BE_IA32_STORE_LEA,  "ia32 Backend transformation: Store merged with a Lea" },
	{ FS_BE_IA32_AM_S,       "ia32 Backend transformation: Source address mode node created" },
	{ FS_BE_IA32_AM_D,       "ia32 Backend transformation: Destination address mode node created" },
	{ FS_BE_IA32_CJMP,       "ia32 Backend transformation: CJmp created to save a cmp/test" },
	{ FS_BE_IA32_2ADDRCPY,   "ia32 Backend transformation: Copy created due to 2-Addresscode constraints" },
	{ FS_BE_IA32_SPILL2ST,   "ia32 Backend transformation: Created Store for a Spill" },
	{ FS_BE_IA32_RELOAD2LD,  "ia32 Backend transformation: Created Load for a Reload" },
	{ FS_BE_IA32_SUB2NEGADD, "ia32 Backend transformation: Created Neg-Add for a Sub due to 2-Addresscode constraints" },
	{ FS_BE_IA32_LEA2ADD,    "ia32 Backend transformation: Transformed Lea back into Add" },
};

static const char *if_conv_names[IF_RESULT_LAST] = {
	"if conv done             ",
	"if conv side effect      ",
	"if conv Phi node found   ",
	"if conv to deep DAG's    ",
	"if conv bad control flow ",
	"if conv denied by arch   ",
};

/**
 * dumps a opcode hash into human readable form
 */
static void simple_dump_opcode_hash(dumper_t *dmp, pset *set)
{
	node_entry_t *entry;
	counter_t f_alive;
	counter_t f_new_node;
	counter_t f_Id;

	cnt_clr(&f_alive);
	cnt_clr(&f_new_node);
	cnt_clr(&f_Id);

	fprintf(dmp->f, "%-16s %-8s %-8s %-8s\n", "Opcode", "alive", "created", "->Id");
	foreach_pset(set, entry) {
		fprintf(dmp->f, "%-16s %8u %8u %8u\n",
			get_id_str(entry->op->name),
			cnt_to_uint(&entry->cnt_alive),
			cnt_to_uint(&entry->new_node),
			cnt_to_uint(&entry->into_Id)
		);

		cnt_add(&f_alive,    &entry->cnt_alive);
		cnt_add(&f_new_node, &entry->new_node);
		cnt_add(&f_Id,       &entry->into_Id);
	}
	fprintf(dmp->f, "-------------------------------------------\n");
	fprintf(dmp->f, "%-16s %8u %8u %8u\n", "Sum",
		cnt_to_uint(&f_alive),
		cnt_to_uint(&f_new_node),
		cnt_to_uint(&f_Id)
	);
}

/**
 * dumps an optimization hash into human readable form
 */
static void simple_dump_opt_hash(dumper_t *dmp, pset *set, int index)
{
	assert(index < ARR_SIZE(opt_names) && "index out of range");
	assert(opt_names[index].kind == index && "opt_names broken");

	if (pset_count(set) > 0) {
		opt_entry_t *entry;

		fprintf(dmp->f, "\n%s:\n", opt_names[index].name);
		fprintf(dmp->f, "%-16s %-8s\n", "Opcode", "deref");

		foreach_pset(set, entry) {
			fprintf(dmp->f, "%-16s %8u\n",
				get_id_str(entry->op->name), cnt_to_uint(&entry->count));
		}
	}
}

/**
 * dumps the register pressure for each block and for each register class
 */
static void simple_dump_be_block_reg_pressure(dumper_t *dmp, graph_entry_t *entry)
{
	be_block_entry_t     *b_entry = pset_first(entry->be_block_hash);
	reg_pressure_entry_t *rp_entry;

	/* return if no be statistic information available */
	if (! b_entry)
		return;

	fprintf(dmp->f, "\nREG PRESSURE:\n");
	fprintf(dmp->f, "%12s", "Block Nr");

	/* print table head (register class names) */
	foreach_pset(b_entry->reg_pressure, rp_entry)
		fprintf(dmp->f, "%15s", rp_entry->class_name);
	fprintf(dmp->f, "\n");

	/* print the reg pressure for all blocks and register classes */
	for (/* b_entry is already initialized */ ;
	     b_entry;
	     b_entry = pset_next(entry->be_block_hash)) {
		fprintf(dmp->f, "BLK   %6ld", b_entry->block_nr);

		foreach_pset(b_entry->reg_pressure, rp_entry)
			fprintf(dmp->f, "%15d", rp_entry->pressure);
		fprintf(dmp->f, "\n");
	}
}

/** prints a distribution entry */
void dump_block_sched_ready_distrib(const distrib_entry_t *entry, void *env)
{
	FILE *dmp_f = env;
	fprintf(dmp_f, "%12d", cnt_to_uint(&entry->cnt));
}

/**
 * dumps the distribution of the amount of ready nodes for each block
 */
static void simple_dump_be_block_sched_ready(dumper_t *dmp, graph_entry_t *entry)
{
	if (pset_count(entry->be_block_hash) > 0) {
		be_block_entry_t *b_entry;
		int              i;

		fprintf(dmp->f, "\nSCHEDULING: NUMBER OF READY NODES\n");
		fprintf(dmp->f, "%12s %12s %12s %12s %12s %12s %12s\n",
			"Block Nr", "1 node", "2 nodes", "3 nodes", "4 nodes", "5 or more", "AVERAGE");

		foreach_pset(entry->be_block_hash, b_entry) {
			/* this ensures that all keys from 1 to 5 are in the table */
			for (i = 1; i < 6; ++i)
				stat_insert_int_distrib_tbl(b_entry->sched_ready, i);

			fprintf(dmp->f, "BLK   %6ld", b_entry->block_nr);
			stat_iterate_distrib_tbl(b_entry->sched_ready, dump_block_sched_ready_distrib, dmp->f);
			fprintf(dmp->f, "%12.2lf", stat_calc_avg_distrib_tbl(b_entry->sched_ready));
			fprintf(dmp->f, "\n");
		}
	}
}

/**
 * dumps permutation statistics for one and block and one class
 */
static void simple_dump_be_block_permstat_class(dumper_t *dmp, perm_class_entry_t *entry)
{
	perm_stat_entry_t *ps_ent;

	fprintf(dmp->f, "%12s %12s %12s %12s\n", "size", "real size", "# chains", "# cycles");
	foreach_pset(entry->perm_stat, ps_ent) {
		fprintf(dmp->f, "%12d %12d %12d %12d\n",
			ps_ent->size,
			ps_ent->real_size,
			stat_get_count_distrib_tbl(ps_ent->chains),
			stat_get_count_distrib_tbl(ps_ent->cycles)
		);
	}
}

/**
 * dumps statistics about perms
 */
static void simple_dump_be_block_permstat(dumper_t *dmp, graph_entry_t *entry)
{
	if (pset_count(entry->be_block_hash) > 0) {
		be_block_entry_t *b_entry;

		fprintf(dmp->f, "\nPERMUTATION STATISTICS BEGIN:\n");
		foreach_pset(entry->be_block_hash, b_entry) {
			perm_class_entry_t *pc_ent;

			fprintf(dmp->f, "BLOCK %ld:\n", b_entry->block_nr);

			if (b_entry->perm_class_stat)
				foreach_pset(b_entry->perm_class_stat, pc_ent) {
					fprintf(dmp->f, "register class %s:\n", pc_ent->class_name);
					simple_dump_be_block_permstat_class(dmp, pc_ent);
				}
		}

		fprintf(dmp->f, "PERMUTATION STATISTICS END\n");
	}
}

/**
 * dumps the number of real_function_call optimization
 */
static void simple_dump_real_func_calls(dumper_t *dmp, counter_t *cnt)
{
	if (! dmp->f)
		return;

	if (! cnt_eq(cnt, 0)) {
		fprintf(dmp->f, "\nReal Function Calls optimized:\n");
		fprintf(dmp->f, "%-16s %8u\n", "Call", cnt_to_uint(cnt));
	}
}

/**
 * dumps the number of tail_recursion optimization
 */
static void simple_dump_tail_recursion(dumper_t *dmp, unsigned num_tail_recursion)
{
	if (! dmp->f)
		return;

	if (num_tail_recursion > 0) {
		fprintf(dmp->f, "\nTail recursion optimized:\n");
		fprintf(dmp->f, "%-16s %8u\n", "Call", num_tail_recursion);
	}
}

/**
 * dumps the edges count
 */
static void simple_dump_edges(dumper_t *dmp, counter_t *cnt)
{
	if (! dmp->f)
		return;

	fprintf(dmp->f, "%-16s %8d\n", "Edges", cnt_to_uint(cnt));
}

/**
 * dumps the IRG
 */
static void simple_dump_graph(dumper_t *dmp, graph_entry_t *entry)
{
	int i, dump_opts = 1;
	block_entry_t *b_entry;
	extbb_entry_t *eb_entry;

	if (! dmp->f)
		return;

	if (entry->irg) {
		ir_graph *const_irg = get_const_code_irg();

		if (entry->irg == const_irg)
			fprintf(dmp->f, "\nConst code Irg %p", (void *)entry->irg);
		else {
			if (entry->ent)
				fprintf(dmp->f, "\nEntity %s, Irg %p", get_entity_ld_name(entry->ent), (void *)entry->irg);
			else
				fprintf(dmp->f, "\nIrg %p", (void *)entry->irg);
		}

		fprintf(dmp->f, " %swalked %u over blocks %u:\n"
			" was inlined               : %u\n"
			" got inlined               : %u\n"
			" strength red              : %u\n"
			" leaf function             : %s\n"
			" calls only leaf functions : %s\n"
			" recursive                 : %s\n"
			" chain call                : %s\n"
			" calls                     : %u\n"
			" indirect calls            : %u\n",
			entry->is_deleted ? "DELETED " : "",
			cnt_to_uint(&entry->cnt_walked), cnt_to_uint(&entry->cnt_walked_blocks),
			cnt_to_uint(&entry->cnt_was_inlined),
			cnt_to_uint(&entry->cnt_got_inlined),
			cnt_to_uint(&entry->cnt_strength_red),
			entry->is_leaf ? "YES" : "NO",
			entry->is_leaf_call == LCS_NON_LEAF_CALL ? "NO" : (entry->is_leaf_call == LCS_LEAF_CALL ? "Yes" : "Maybe"),
			entry->is_recursive ? "YES" : "NO",
			entry->is_chain_call ? "YES" : "NO",
			cnt_to_uint(&entry->cnt_all_calls),
			cnt_to_uint(&entry->cnt_indirect_calls)
		);

		for (i = 0; i < sizeof(entry->cnt_if_conv)/sizeof(entry->cnt_if_conv[0]); ++i) {
			fprintf(dmp->f, " %s : %u\n", if_conv_names[i], cnt_to_uint(&entry->cnt_if_conv[i]));
		}
	}
	else {
		fprintf(dmp->f, "\nGlobals counts:\n");
		fprintf(dmp->f, "--------------\n");
		dump_opts = 0;
	}

	simple_dump_opcode_hash(dmp, entry->opcode_hash);
	simple_dump_edges(dmp, &entry->cnt_edges);

	/* effects of optimizations */
	if (dump_opts) {
		int i;

		simple_dump_real_func_calls(dmp, &entry->cnt_real_func_call);
		simple_dump_tail_recursion(dmp, entry->num_tail_recursion);

		for (i = 0; i < sizeof(entry->opt_hash)/sizeof(entry->opt_hash[0]); ++i) {
			simple_dump_opt_hash(dmp, entry->opt_hash[i], i);
		}

		/* dump block info */
		fprintf(dmp->f, "\n%12s %12s %12s %12s %12s %12s %12s\n", "Block Nr", "Nodes", "intern E", "incoming E", "outgoing E", "Phi", "quot");
		foreach_pset(entry->block_hash, b_entry) {
			fprintf(dmp->f, "BLK   %6ld %12u %12u %12u %12u %12u %4.8f\n",
				b_entry->block_nr,
				cnt_to_uint(&b_entry->cnt_nodes),
				cnt_to_uint(&b_entry->cnt_edges),
				cnt_to_uint(&b_entry->cnt_in_edges),
				cnt_to_uint(&b_entry->cnt_out_edges),
				cnt_to_uint(&b_entry->cnt_phi_data),
				cnt_to_dbl(&b_entry->cnt_edges) / cnt_to_dbl(&b_entry->cnt_nodes)
			);
		}

		/* dump block reg pressure */
		simple_dump_be_block_reg_pressure(dmp, entry);

		/* dump block ready nodes distribution */
		simple_dump_be_block_sched_ready(dmp, entry);

		/* dump block permutation statistics */
		simple_dump_be_block_permstat(dmp, entry);

		if (dmp->status->stat_options & FIRMSTAT_COUNT_EXTBB) {
			/* dump extended block info */
			fprintf(dmp->f, "\n%12s %12s %12s %12s %12s %12s %12s\n", "Extbb Nr", "Nodes", "intern E", "incoming E", "outgoing E", "Phi", "quot");
			foreach_pset(entry->extbb_hash, eb_entry) {
				fprintf(dmp->f, "ExtBB %6ld %12u %12u %12u %12u %12u %4.8f\n",
					eb_entry->block_nr,
					cnt_to_uint(&eb_entry->cnt_nodes),
					cnt_to_uint(&eb_entry->cnt_edges),
					cnt_to_uint(&eb_entry->cnt_in_edges),
					cnt_to_uint(&eb_entry->cnt_out_edges),
					cnt_to_uint(&eb_entry->cnt_phi_data),
					cnt_to_dbl(&eb_entry->cnt_edges) / cnt_to_dbl(&eb_entry->cnt_nodes)
				);
			}
		}
	}
}

/**
 * dumps the IRG
 */
static void simple_dump_const_tbl(dumper_t *dmp, const constant_info_t *tbl)
{
	int i;
	counter_t sum;

	if (! dmp->f)
		return;

	cnt_clr(&sum);

	fprintf(dmp->f, "\nConstant Information:\n");
	fprintf(dmp->f, "---------------------\n");

	fprintf(dmp->f, "\nBit usage for integer constants\n");
	fprintf(dmp->f, "-------------------------------\n");

	for (i = 0; i < ARR_SIZE(tbl->int_bits_count); ++i) {
		fprintf(dmp->f, "%5d %12u\n", i + 1, cnt_to_uint(&tbl->int_bits_count[i]));
		cnt_add(&sum, &tbl->int_bits_count[i]);
	}
	fprintf(dmp->f, "-------------------------------\n");

	fprintf(dmp->f, "\nFloating point constants classification\n");
	fprintf(dmp->f, "--------------------------------------\n");
	for (i = 0; i < ARR_SIZE(tbl->floats); ++i) {
		fprintf(dmp->f, "%-10s %12u\n", stat_fc_name(i), cnt_to_uint(&tbl->floats[i]));
		cnt_add(&sum, &tbl->floats[i]);
	}
	fprintf(dmp->f, "--------------------------------------\n");

	fprintf(dmp->f, "other %12u\n", cnt_to_uint(&tbl->others));
	cnt_add(&sum, &tbl->others);
	fprintf(dmp->f, "-------------------------------\n");

	fprintf(dmp->f, "sum   %12u\n", cnt_to_uint(&sum));
}

/**
 * initialize the simple dumper
 */
static void simple_init(dumper_t *dmp, const char *name)
{
	char fname[2048];

	snprintf(fname, sizeof(fname), "%s.txt", name);
	dmp->f = fopen(fname, "w");
	if (! dmp->f) {
		perror(fname);
	}
}

/**
 * finishes the simple dumper
 */
static void simple_finish(dumper_t *dmp)
{
	if (dmp->f)
		fclose(dmp->f);
	dmp->f = NULL;
}

/**
 * the simple human readable dumper
 */
const dumper_t simple_dumper = {
	simple_dump_graph,
	simple_dump_const_tbl,
	simple_init,
	simple_finish,
	NULL,
	NULL,
	NULL,
};

/* ---------------------------------------------------------------------- */

/**
 * count the nodes as needed:
 *
 * 1 normal (data) Phi's
 * 2 memory Phi's
 * 3 Proj
 * 0 all other nodes
 */
static void csv_count_nodes(dumper_t *dmp, graph_entry_t *graph, counter_t cnt[])
{
	node_entry_t *entry;
	int i;

	for (i = 0; i < 4; ++i)
		cnt_clr(&cnt[i]);

	foreach_pset(graph->opcode_hash, entry) {
		if (entry->op == op_Phi) {
			/* normal Phi */
			cnt_add(&cnt[1], &entry->cnt_alive);
		}
		else if (entry->op == dmp->status->op_PhiM) {
			/* memory Phi */
			cnt_add(&cnt[2], &entry->cnt_alive);
		}
		else if (entry->op == op_Proj) {
			/* Proj */
			cnt_add(&cnt[3], &entry->cnt_alive);
		}
		else {
			/* all other nodes */
			cnt_add(&cnt[0], &entry->cnt_alive);
		}
	}
}

/**
 * dumps the IRG
 */
static void csv_dump_graph(dumper_t *dmp, graph_entry_t *entry)
{
	const char *name;
	counter_t cnt[4];

	if (! dmp->f)
		return;

	if (entry->irg && !entry->is_deleted) {
		ir_graph *const_irg = get_const_code_irg();

		if (entry->irg == const_irg) {
			name = "<Const code Irg>";
			return;
		}
		else {
			if (entry->ent)
				name = get_entity_name(entry->ent);
			else
				name = "<UNKNOWN IRG>";
		}

		csv_count_nodes(dmp, entry, cnt);

		fprintf(dmp->f, "%-40s, %p, %d, %d, %d, %d\n",
			name,
			(void *)entry->irg,
			cnt_to_uint(&cnt[0]),
			cnt_to_uint(&cnt[1]),
			cnt_to_uint(&cnt[2]),
			cnt_to_uint(&cnt[3])
		);
	}
}

/**
 * dumps the IRG
 */
static void csv_dump_const_tbl(dumper_t *dmp, const constant_info_t *tbl)
{
	/* FIXME: NYI */
}

/**
 * initialize the simple dumper
 */
static void csv_init(dumper_t *dmp, const char *name)
{
	char fname[2048];

	snprintf(fname, sizeof(fname), "%s.csv", name);
	dmp->f = fopen(fname, "a");
	if (! dmp->f)
		perror(fname);
}

/**
 * finishes the simple dumper
 */
static void csv_finish(dumper_t *dmp)
{
	if (dmp->f)
		fclose(dmp->f);
	dmp->f = NULL;
}

/**
 * the simple human readable dumper
 */
const dumper_t csv_dumper = {
	csv_dump_graph,
	csv_dump_const_tbl,
	csv_init,
	csv_finish,
	NULL,
	NULL,
	NULL,
};
