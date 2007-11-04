/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
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
 * @brief   Write vcg representation of firm to file.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Hubert Schmidt
 * @version $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <stdarg.h>

#include "irdump_t.h"

#include "firm_common_t.h"

#include "irgraph_t.h"
#include "irprog_t.h"
#include "entity_t.h"
#include "trouts.h"
#include "irgwalk.h"
#include "tv_t.h"

#include "irdom.h"
#include "field_temperature.h"

#define MY_SIZE 1024     /* Size of an array that actually should be computed. */

/**
 * Just opens a file, mangling a file name.
 *
 * The file name results from the concatenation of the following parts:
 *
 * @param basename  The basis of the name telling about the content.
 * @param suffix1   The first suffix.
 * @param suffix2   The second suffix.
 * @param suffix3   The third suffix.
 */
static FILE *text_open(const char *basename, const char * suffix1, const char *suffix2, const char *suffix3) {
	FILE *F;
	int len = strlen(basename), i, j;
	char *fname;  /* filename to put the vcg information in */

	if (!basename) assert(basename);
	if (!suffix1) suffix1 = "";
	if (!suffix2) suffix2 = "";
	if (!suffix3) suffix3 = ".txt";

	/* open file for vcg graph */
	fname = xmalloc(strlen(basename)*2 + strlen(suffix1) + strlen(suffix2) + 5); /* *2: space for escapes. */

	j = 0;
	for (i = 0; i < len; ++i) {  /* replace '/' in the name: escape by @. */
		if (basename[i] == '/') {
			fname[j] = '@'; j++; fname[j] = '1'; j++;
		} else if (basename[i] == '@') {
			fname[j] = '@'; j++; fname[j] = '2'; j++;
		} else {
			fname[j] = basename[i]; j++;
		}
	}
	fname[j] = '\0';
	strcat(fname, suffix1);  /* append file suffix */
	strcat(fname, suffix2);  /* append file suffix */
	strcat(fname, suffix3);  /* append the .txt suffix */

	F = fopen(fname, "w");   /* open file for writing */
	if (!F) {
		perror(fname);
		assert(0);
	}
	free(fname);

	return F;
}

/* Write the irnode and all its attributes to the file passed. */
int dump_irnode_to_file(FILE *F, ir_node *n) {
	int i, bad = 0;
	char comma;
	ir_graph *irg;

	dump_node_opcode(F, n);
	fprintf(F, " %ld\n", get_irn_node_nr(n));

	fprintf(F, "  index: %u\n", get_irn_idx(n));
	if (opt_dump_pointer_values_to_info)
		fprintf (F, "  addr:    %p \n", (void *)n);
	fprintf (F, "  mode:    %s\n", get_mode_name(get_irn_mode(n)));
	fprintf (F, "  visited: %ld \n", get_irn_visited(n));
	irg = get_irn_irg(n);
	if (irg != get_const_code_irg())
		fprintf (F, "  irg:     %s\n", get_ent_dump_name(get_irg_entity(irg)));

	if (get_irn_pinned(n) == op_pin_state_floats &&
		get_irg_pinned(get_irn_irg(n)) == op_pin_state_floats) {
		fprintf(F, "  node was pinned in ");
		dump_node_opcode(F, get_irn_n(n, -1));
		fprintf(F, " %ld\n", get_irn_node_nr(get_irn_n(n, -1)));
	}

#ifdef INTERPROCEDURAL_VIEW
	fprintf(F, "  arity:   %d\n", get_irn_intra_arity(n));
	/* show all predecessor nodes */
	fprintf(F, "  pred nodes: \n");
	if (!is_Block(n)) {
		fprintf(F, "    -1:    ");
		dump_node_opcode(F, get_irn_n(n, -1));
		fprintf(F, " %ld\n", get_irn_node_nr(get_irn_n(n, -1)));
	}
	for ( i = 0; i < get_irn_intra_arity(n); ++i) {
		fprintf(F, "     %d: %s ", i, is_intra_backedge(n, i) ? "be" : "  ");
		dump_node_opcode(F, get_irn_intra_n(n, i));
		fprintf(F, " %ld\n", get_irn_node_nr(get_irn_intra_n(n, i)));
	}
#else
	fprintf(F, "  arity:   %d\n", get_irn_arity(n));
	/* show all predecessor nodes */
	fprintf(F, "  pred nodes: \n");
	if (!is_Block(n)) {
		fprintf(F, "    -1:    ");
		dump_node_opcode(F, get_irn_n(n, -1));
		fprintf(F, " %ld\n", get_irn_node_nr(get_irn_n(n, -1)));
	}
	for ( i = 0; i < get_irn_arity(n); ++i) {
		fprintf(F, "     %d: %s ", i, is_backedge(n, i) ? "be" : "  ");
		dump_node_opcode(F, get_irn_n(n, i));
		fprintf(F, " %ld\n", get_irn_node_nr(get_irn_n(n, i)));
	}
#endif

	fprintf(F, "  Private Attributes:\n");

	if (get_irn_opcode(n) == iro_Proj)
		fprintf(F, "  proj nr: %ld\n", get_Proj_proj(n));

#ifdef INTERPROCEDURAL_VIEW
	if ((get_irp_ip_view_state() != ip_view_no)
		&& (get_irn_opcode(n) == iro_Filter || get_irn_opcode(n) == iro_Block)) {
		fprintf(F, "  inter arity: %d\n", get_irn_inter_arity(n));
		fprintf(F, "  inter pred nodes: \n");
		for ( i = 0; i < get_irn_inter_arity(n); ++i) {
			fprintf(F, "     %d: %s ", i, is_intra_backedge(n, i) ? "be" : "  ");
			dump_node_opcode(F, get_irn_inter_n(n, i));
			fprintf(F, " %ld\n", get_irn_node_nr(get_irn_inter_n(n, i)));
		}
	}
#endif

	if (is_fragile_op(n)) {
		fprintf(F, "  pinned state: %s\n", get_op_pin_state_name(get_irn_pinned(n)));
		/* not dumped: frag array */
	}

	/* This is not nice, output it as a marker in the predecessor list. */
	if ((get_irn_op(n) == op_Block) ||
		(get_irn_op(n) == op_Phi) ||
		((get_irn_op(n) == op_Filter) && get_interprocedural_view())) {
		fprintf(F, "  backedges:");
		comma = ' ';
		for (i = 0; i < get_irn_arity(n); i++)
			if (is_backedge(n, i)) { fprintf(F, "%c %d", comma, i); comma = ','; }
			fprintf(F, "\n");
	}

	/* Loop node.   Someone else please tell me what's wrong ... */
	if (get_irn_loop(n)) {
		ir_loop *loop = get_irn_loop(n);
		assert(loop);
		fprintf(F, "  in loop %d with depth %d\n",
			get_loop_loop_nr(loop), get_loop_depth(loop));
	}

	/* Source types */
	switch (get_irn_opcode(n)) {
	case iro_Block: {
		fprintf(F, "  block visited: %ld\n", get_Block_block_visited(n));
		if (get_irg_dom_state(get_irn_irg(n)) != dom_none) {
			fprintf(F, "  dom depth %d\n", get_Block_dom_depth(n));
			fprintf(F, "  tree pre num %d\n", get_Block_dom_tree_pre_num(n));
			fprintf(F, "  max subtree pre num %d\n", get_Block_dom_max_subtree_pre_num(n));
		}

		fprintf(F, "  Execution freqency statistics:\n");
		if (get_irg_exec_freq_state(get_irn_irg(n)) != exec_freq_none)
			fprintf(F, "    procedure local evaluation:   %8.2lf\n", get_irn_exec_freq(n));
#ifdef INTERPROCEDURAL_VIEW
		if (get_irp_loop_nesting_depth_state() != loop_nesting_depth_none)
			fprintf(F, "    call freqency of procedure:   %8.2lf\n",
			get_irg_method_execution_frequency(get_irn_irg(n)));
		if (get_irp_callgraph_state() == irp_callgraph_and_calltree_consistent)
			fprintf(F, "    recursion depth of procedure: %8.2lf\n", (double)get_irn_recursion_depth(n));
		if ((get_irg_exec_freq_state(get_irn_irg(n)) != exec_freq_none) &&
			(get_irp_loop_nesting_depth_state() != loop_nesting_depth_none) &&
			(get_irp_callgraph_state() == irp_callgraph_and_calltree_consistent))
			fprintf(F, "    final evaluation:           **%8.2lf**\n", get_irn_final_cost(n));
#endif
    if (has_Block_label(n))
      fprintf(F, "    Label: %lu\n", get_Block_label(n));

		/* not dumped: graph_arr */
		/* not dumped: mature    */
	}  break;
	case iro_Start: {
		ir_type *tp = get_entity_type(get_irg_entity(get_irn_irg(n)));
		fprintf(F, "  start of method of type %s \n", get_type_name_ex(tp, &bad));
		for (i = 0; i < get_method_n_params(tp); ++i)
			fprintf(F, "    param %d type: %s \n", i, get_type_name_ex(get_method_param_type(tp, i), &bad));
#ifdef INTERPROCEDURAL_VIEW
		if ((get_irp_ip_view_state() == ip_view_valid) && !get_interprocedural_view()) {
			ir_node *sbl = get_nodes_block(n);
			int i, n_cfgpreds = get_Block_cg_n_cfgpreds(sbl);
			fprintf(F, "  graph has %d interprocedural predecessors:\n", n_cfgpreds);
			for (i = 0; i < n_cfgpreds; ++i) {
				ir_node *cfgpred = get_Block_cg_cfgpred(sbl, i);
				fprintf(F, "    %d: Call %ld in graph %s\n", i, get_irn_node_nr(cfgpred),
					get_irg_dump_name(get_irn_irg(cfgpred)));
			}
		}
#endif
	} break;
	case iro_Cond: {
		fprintf(F, "  condition kind: %s\n",  get_Cond_kind(n) == dense ? "dense" : "fragmentary");
		fprintf(F, "  default ProjNr: %ld\n", get_Cond_defaultProj(n));
		if (get_Cond_jmp_pred(n) != COND_JMP_PRED_NONE)
			fprintf(F, "  jump prediction: %s\n", get_cond_jmp_predicate_name(get_Cond_jmp_pred(n)));
	} break;
	case iro_Alloc: {
		fprintf(F, "  allocating entity of type: %s \n", get_type_name_ex(get_Alloc_type(n), &bad));
		fprintf(F, "  allocating on: the %s\n", (get_Alloc_where(n) == stack_alloc) ? "stack" : "heap");
	} break;
	case iro_Free: {
		fprintf(F, "  freeing entity of type %s \n", get_type_name_ex(get_Free_type(n), &bad));
		fprintf(F, "  allocated on: the %s\n", (get_Free_where(n) == stack_alloc) ? "stack" : "heap");
	} break;
	case iro_Sel: {
		ir_entity *ent = get_Sel_entity(n);
		if (ent) {
			fprintf(F, "  Selecting entity %s (%ld)\n", get_entity_name(ent), get_entity_nr(ent));
			fprintf(F, "    of type    %s\n",  get_type_name_ex(get_entity_type(ent),  &bad));
			fprintf(F, "    with owner %s.\n", get_type_name_ex(get_entity_owner(ent), &bad));
		}
		else {
			fprintf(F, "  <NULL entity>\n");
			bad = 1;
		}
	} break;
	case iro_Call: {
		ir_type *tp = get_Call_type(n);
		fprintf(F, "  calling method of type %s \n", get_type_name_ex(tp, &bad));
		if(get_unknown_type() != tp) {
			for (i = 0; i < get_method_n_params(tp); ++i)
				fprintf(F, "    param %d type: %s \n", i, get_type_name_ex(get_method_param_type(tp, i), &bad));
			for (i = 0; i < get_method_n_ress(tp); ++i)
				fprintf(F, "    resul %d type: %s \n", i, get_type_name_ex(get_method_res_type(tp, i), &bad));
		}
		if (Call_has_callees(n)) {
			fprintf(F, "  possible callees: \n");
			for (i = 0; i < get_Call_n_callees(n); i++) {
				fprintf(F, "    %d: %s\n", i, get_ent_dump_name(get_Call_callee(n, i)));
			}
		}
	} break;
	case iro_CallBegin: {
		ir_node *call = get_CallBegin_call(n);
		fprintf(F, "  Call: %ld\n", get_irn_node_nr(call));
		if (Call_has_callees(call)) {
			fprintf(F, "  possible callees: \n");
			for (i = 0; i < get_Call_n_callees(call); i++) {
				fprintf(F, "    %d: %s\n", i, get_ent_dump_name(get_Call_callee(call, i)));
			}
		}
	} break;
	case iro_Cast: {
		fprintf(F, "  cast to type: %s\n", get_type_name_ex(get_Cast_type(n), &bad));
	} break;
	case iro_Return: {
		if (!get_interprocedural_view()) {
			ir_type *tp = get_entity_type(get_irg_entity(get_irn_irg(n)));
			fprintf(F, "  return in method of type %s \n", get_type_name_ex(tp, &bad));
			for (i = 0; i < get_method_n_ress(tp); ++i)
				fprintf(F, "    res %d type: %s \n", i, get_type_name_ex(get_method_res_type(tp, i), &bad));
		}
	} break;
	case iro_Const: {
		assert(get_Const_type(n) != firm_none_type);
		fprintf(F, "  Const of type %s \n", get_type_name_ex(get_Const_type(n), &bad));
	} break;
	case iro_SymConst: {
		switch(get_SymConst_kind(n)) {
		case symconst_addr_name:
			fprintf(F, "  kind: addr_name\n");
			fprintf(F, "  name: %s\n", get_id_str(get_SymConst_name(n)));
			break;
		case symconst_addr_ent:
			fprintf(F, "  kind:   addr_ent\n");
			fprintf(F, "  entity: ");
			dump_entity_to_file(F, get_SymConst_entity(n), dump_verbosity_onlynames);
			break;
		case symconst_ofs_ent:
			fprintf(F, "  kind:   offset\n");
			fprintf(F, "  entity: ");
			dump_entity_to_file(F, get_SymConst_entity(n), dump_verbosity_onlynames);
			break;
		case symconst_type_tag:
			fprintf(F, "  kind: type_tag\n");
			fprintf(F, "  type: ");
			dump_type_to_file(F, get_SymConst_type(n), dump_verbosity_onlynames);
			break;
		case symconst_type_size:
			fprintf(F, "  kind: size\n");
			fprintf(F, "  type: ");
			dump_type_to_file(F, get_SymConst_type(n), dump_verbosity_onlynames);
			break;
		case symconst_type_align:
			fprintf(F, "  kind: alignment\n");
			fprintf(F, "  type: ");
			dump_type_to_file(F, get_SymConst_type(n), dump_verbosity_onlynames);
			break;
		case symconst_enum_const:
			fprintf(F, "  kind: enumeration\n");
			fprintf(F, "  name: %s\n", get_enumeration_name(get_SymConst_enum(n)));
			break;
		case symconst_label:
			fprintf(F, "  kind: label\n");
			fprintf(F, "  label: %lu\n", get_SymConst_label(n));
			break;
		}
		fprintf(F, "  type of value: %s \n", get_type_name_ex(get_SymConst_value_type(n), &bad));
	} break;
	case iro_Load:
		fprintf(F, "  mode of loaded value: %s\n", get_mode_name_ex(get_Load_mode(n), &bad));
		fprintf(F, "  volatility: %s\n", get_volatility_name(get_Load_volatility(n)));
		fprintf(F, "  align: %s\n", get_align_name(get_Load_align(n)));
		break;
	case iro_Store:
		fprintf(F, "  volatility: %s\n", get_volatility_name(get_Store_volatility(n)));
		fprintf(F, "  align: %s\n", get_align_name(get_Store_align(n)));
		break;
	case iro_Confirm:
		fprintf(F, "  compare operation: %s\n", get_pnc_string(get_Confirm_cmp(n)));
		break;
	case iro_ASM: {
		const ir_asm_constraint *cons;
		ident **clobber;
		int l;

		fprintf(F, "  assembler text: %s", get_id_str(get_ASM_text(n)));
		l = get_ASM_n_input_constraints(n);
		if (l > 0) {
			fprintf(F, "\n  inputs:  ");
			cons = get_ASM_input_constraints(n);
			for (i = 0; i < l; ++i)
				fprintf(F, "%%%u %s ", cons[i].pos, get_id_str(cons[i].constraint));
		}
		l = get_ASM_n_output_constraints(n);
		if (l > 0) {
			fprintf(F, "\n  outputs: ");
			cons = get_ASM_output_constraints(n);
			for (i = 0; i < l; ++i)
				fprintf(F, "%%%u %s ", cons[i].pos, get_id_str(cons[i].constraint));
		}
		l = get_ASM_n_clobbers(n);
		if (l > 0) {
			fprintf(F, "\n  clobber: ");
			clobber = get_ASM_clobbers(n);
			for (i = 0; i < l; ++i)
				fprintf(F, "%s ", get_id_str(clobber[i]));
		}
		if (get_irn_pinned(n) != op_pin_state_floats)
			fprintf(F, "\n  volatile");
		fprintf(F, "\n");
	} break;
	default: ;
	}

	if (get_irg_typeinfo_state(get_irn_irg(n)) == ir_typeinfo_consistent  ||
		get_irg_typeinfo_state(get_irn_irg(n)) == ir_typeinfo_inconsistent  )
		if (get_irn_typeinfo_type(n) != firm_none_type)
			fprintf (F, "  Analysed type: %s\n", get_type_name_ex(get_irn_typeinfo_type(n), &bad));

	return bad;
}



void dump_irnode(ir_node *n) {
	dump_irnode_to_file(stdout, n);
}


void dump_graph_to_file(FILE *F, ir_graph *irg) {
	fprintf(F, "graph %s\n", get_irg_dump_name(irg));
}

void dump_graph(ir_graph *g) {
	dump_graph_to_file(stdout, g);
}

static void dump_node_to_graph_file(ir_node *n, void *env) {
	FILE *F = (FILE *)env;

	dump_irnode_to_file(F, n);
	fprintf(F, "\n");
}

void dump_graph_as_text(ir_graph *irg, const char *suffix) {
	const char *basename = get_irg_dump_name(irg);
	FILE *F;

	F = text_open(basename, suffix, "", ".txt");

	dump_graph_to_file(F, irg);
	fprintf(F, "\n\n");
	irg_walk_graph(irg, NULL, dump_node_to_graph_file, F);

	fclose (F);
}

#ifdef EXTENDED_ACCESS_STATS
static int addr_is_alloc(ir_node *acc) {
	ir_node *addr = NULL;
	ir_opcode addr_op;
	if (is_memop(acc)) {
		addr = get_memop_ptr(acc);
	} else {
		assert(is_Call(acc));
		addr = get_Call_ptr(acc);
	}

	addr_op = get_irn_opcode(addr);

	while (addr_op != iro_Alloc) {
		switch (addr_op) {
		case iro_Sel:
			addr = get_Sel_ptr(addr);
			break;
		case iro_Cast:
			addr = get_Cast_op(addr);
			break;
		case iro_Proj:
			addr = get_Proj_pred(addr);
			break;
		case iro_SymConst:
		case iro_Const:
			return 0;
			break;
		case iro_Phi:
		case iro_Load:
		case iro_Call:
		case iro_Start:
			return 0;
			break;

		default:
			//DDMN(addr);
			//assert(0 && "unexpected address node");
			;
		}
		addr_op = get_irn_opcode(addr);
	}

	/* In addition, the alloc must be in the same loop. */
	return 1;
}
#endif

/** dumps something like:
 *
 *  "prefix"  "Name" (x): node1, ... node7,\n
 *  "prefix"    node8, ... node15,\n
 *  "prefix"    node16, node17\n
 */
static void dump_node_list(FILE *F, firm_kind *k, char *prefix,
                           int (*get_entity_n_nodes)(firm_kind *ent),
                           ir_node *(*get_entity_node)(firm_kind *ent, int pos),
                           char *name) {
	int i, n_nodes = get_entity_n_nodes(k);
	char *comma = "";

	fprintf(F, "%s  %s (%d):", prefix, name, n_nodes);
	for (i = 0; i < n_nodes; ++i) {
		int rem;
		if (i > 7 && !(i & 7)) { /* line break every eight node. */
			fprintf(F, ",\n%s   ", prefix);
			comma = "";
		}
		fprintf(F, "%s ", comma);
		rem = opt_dump_analysed_type_info;
		opt_dump_analysed_type_info = 0;
		dump_node_label(F, get_entity_node(k, i));
		opt_dump_analysed_type_info = rem;
		comma = ",";
	}
	fprintf(F, "\n");
}

/** dumps something like:
 *
 *  "prefix"  "Name" (x): node1, ... node7,\n
 *  "prefix"    node8, ... node15,\n
 *  "prefix"    node16, node17\n
 */
static void dump_type_list(FILE *F, ir_type *tp, char *prefix,
                           int (*get_n_types)(ir_type *tp),
                           ir_type *(*get_type)(ir_type *tp, int pos),
                           char *name) {
	int i, n_nodes = get_n_types(tp);
	char *comma = "";

	fprintf(F, "%s  %s (%d):", prefix, name, n_nodes);
	for (i = 0; i < n_nodes; ++i) {
		if (i > 7 && !(i & 7)) { /* line break every eight node. */
			fprintf(F, ",\n%s   ", prefix);
			comma = "";
		}
		fprintf(F, "%s %s(%ld)", comma, get_type_name(get_type(tp, i)), get_type_nr(tp));
		//dump_type_to_file(F, get_type(tp, i), dump_verbosity_onlynames);
		comma = ",";
	}
	fprintf(F, "\n");
}

void dump_entity_to_file_prefix(FILE *F, ir_entity *ent, char *prefix, unsigned verbosity) {
	int i, j;
	ir_type *owner, *type;

	assert(is_entity(ent));
	owner = get_entity_owner(ent);
	type  = get_entity_type(ent);
	if (verbosity & dump_verbosity_onlynames) {
		fprintf(F, "%sentity %s.%s (%ld)\n", prefix, get_type_name(get_entity_owner(ent)),
			get_entity_name(ent), get_entity_nr(ent));
		return;
	}

	if (verbosity & dump_verbosity_entattrs) {
		fprintf(F, "%sentity %s (%ld)\n", prefix, get_entity_name(ent), get_entity_nr(ent));
		fprintf(F, "%s  type:  %s (%ld)\n", prefix, get_type_name(type),  get_type_nr(type));
		fprintf(F, "%s  owner: %s (%ld)\n", prefix, get_type_name(owner), get_type_nr(owner));

		if (is_Class_type(get_entity_owner(ent))) {
			if (get_entity_n_overwrites(ent) > 0) {
				fprintf(F, "%s  overwrites:\n", prefix);
				for (i = 0; i < get_entity_n_overwrites(ent); ++i) {
					ir_entity *ov = get_entity_overwrites(ent, i);
					fprintf(F, "%s    %d: %s of class %s\n", prefix, i, get_entity_name(ov),
						get_type_name(get_entity_owner(ov)));
				}
			} else {
				fprintf(F, "%s  Does not overwrite other entities. \n", prefix);
			}
			if (get_entity_n_overwrittenby(ent) > 0) {
				fprintf(F, "%s  overwritten by:\n", prefix);
				for (i = 0; i < get_entity_n_overwrittenby(ent); ++i) {
					ir_entity *ov = get_entity_overwrittenby(ent, i);
					fprintf(F, "%s    %d: %s of class %s\n", prefix, i, get_entity_name(ov),
						get_type_name(get_entity_owner(ov)));
				}
			} else {
				fprintf(F, "%s  Is not overwritten by other entities. \n", prefix);
			}

			if (get_irp_inh_transitive_closure_state() != inh_transitive_closure_none) {
				ir_entity *ov;
				fprintf(F, "%s  transitive overwrites:\n", prefix);
				for (ov = get_entity_trans_overwrites_first(ent);
				ov;
				ov = get_entity_trans_overwrites_next(ent)) {
					fprintf(F, "%s    : %s of class %s\n", prefix, get_entity_name(ov),
						get_type_name(get_entity_owner(ov)));
				}
				fprintf(F, "%s  transitive overwritten by:\n", prefix);
				for (ov = get_entity_trans_overwrittenby_first(ent);
				ov;
				ov = get_entity_trans_overwrittenby_next(ent)) {
					fprintf(F, "%s    : %s of class %s\n", prefix, get_entity_name(ov),
						get_type_name(get_entity_owner(ov)));
				}
			}
		}

		fprintf(F, "%s  allocation:  %s", prefix, get_allocation_name(get_entity_allocation(ent)));
		fprintf(F, "\n%s  visibility:  %s", prefix, get_visibility_name(get_entity_visibility(ent)));
		fprintf(F, "\n%s  variability: %s", prefix, get_variability_name(get_entity_variability(ent)));

		if (is_Method_type(get_entity_type(ent))) {
			unsigned mask = get_entity_additional_properties(ent);
			unsigned cc   = get_method_calling_convention(get_entity_type(ent));
			ir_graph *irg = get_entity_irg(ent);

			if (irg) {
				fprintf(F, "\n%s  estimated node count: %u", prefix, get_irg_estimated_node_cnt(irg));
				fprintf(F, "\n%s  maximum node index:   %u", prefix, get_irg_last_idx(irg));
			}

			if (mask) {
				fprintf(F, "\n%s  additional prop: ", prefix);

				if (mask & mtp_property_const)    fprintf(F, "const_function, ");
				if (mask & mtp_property_pure)     fprintf(F, "pure_function, ");
				if (mask & mtp_property_noreturn) fprintf(F, "noreturn_function, ");
				if (mask & mtp_property_nothrow)  fprintf(F, "nothrow_function, ");
				if (mask & mtp_property_naked)    fprintf(F, "naked_function, ");
			}
			fprintf(F, "\n%s  calling convention: ", prefix);
			if (cc & cc_reg_param) fprintf(F, "regparam, ");
			if (cc & cc_this_call) fprintf(F, "thiscall, ");
			if (IS_CDECL(cc))
				fprintf(F, "cdecl");
			else if (IS_STDCALL(cc))
				fprintf(F, "stdcall");
			else {
				fprintf(F, (cc & cc_last_on_top) ? "last param on top, " : "first param on top, ");
				fprintf(F, (cc & cc_callee_clear_stk) ? "callee clear stack" : "caller clear stack");
			}
			fprintf(F, "\n%s  vtable number:        %u", prefix, get_entity_vtable_number(ent));
		}

		fprintf(F, "\n");
	} else {  /* no entattrs */
		fprintf(F, "%s(%3d:%d) %-40s: %s", prefix,
			get_entity_offset(ent), get_entity_offset_bits_remainder(ent),
			get_type_name(get_entity_type(ent)), get_entity_name(ent));
		if (is_Method_type(get_entity_type(ent))) fprintf(F, "(...)");

		if (verbosity & dump_verbosity_accessStats) {
			if (get_entity_allocation(ent) == allocation_static) fprintf(F, " (stat)");
			if (get_entity_peculiarity(ent) == peculiarity_description) fprintf(F, " (desc)");
			if (get_entity_peculiarity(ent) == peculiarity_inherited)   fprintf(F, " (inh)");
		}
		fprintf(F, "\n");
	}

	if (verbosity & dump_verbosity_entconsts) {
		if (get_entity_variability(ent) != variability_uninitialized) {
			if (is_atomic_entity(ent)) {
				fprintf(F, "%s  atomic value: ", prefix);
				dump_node_opcode(F, get_atomic_ent_value(ent));
			} else {
				fprintf(F, "%s  compound values:", prefix);
				for (i = 0; i < get_compound_ent_n_values(ent); ++i) {
					compound_graph_path *path = get_compound_ent_value_path(ent, i);
					ir_entity *ent0 = get_compound_graph_path_node(path, 0);
					fprintf(F, "\n%s    %3d:%d ", prefix, get_entity_offset(ent0), get_entity_offset_bits_remainder(ent0));
					if (get_type_state(type) == layout_fixed)
						fprintf(F, "(%3d:%d) ",   get_compound_ent_value_offset_bytes(ent, i), get_compound_ent_value_offset_bit_remainder(ent, i));
					fprintf(F, "%s", get_entity_name(ent));
					for (j = 0; j < get_compound_graph_path_length(path); ++j) {
						ir_entity *node = get_compound_graph_path_node(path, j);
						fprintf(F, ".%s", get_entity_name(node));
						if (is_Array_type(get_entity_owner(node)))
							fprintf(F, "[%d]", get_compound_graph_path_array_index(path, j));
					}
					fprintf(F, "\t = ");
					dump_node_opcode(F, get_compound_ent_value(ent, i));
				}
			}
			fprintf(F, "\n");
		}
	}

	if (verbosity & dump_verbosity_entattrs) {
		fprintf(F, "%s  volatility:  %s", prefix, get_volatility_name(get_entity_volatility(ent)));
		fprintf(F, "\n%s  alignment:  %s", prefix, get_align_name(get_entity_align(ent)));
		fprintf(F, "\n%s  peculiarity: %s", prefix, get_peculiarity_name(get_entity_peculiarity(ent)));
		fprintf(F, "\n%s  ld_name: %s", prefix, ent->ld_name ? get_entity_ld_name(ent) : "no yet set");
		fprintf(F, "\n%s  offset:  %d bytes, %d rem bits", prefix, get_entity_offset(ent), get_entity_offset_bits_remainder(ent));
		if (is_Method_type(get_entity_type(ent))) {
			if (get_entity_irg(ent))   /* can be null */ {
				fprintf(F, "\n%s  irg = %ld", prefix, get_irg_graph_nr(get_entity_irg(ent)));
#ifdef INTERPROCEDURAL_VIEW
				if (get_irp_callgraph_state() == irp_callgraph_and_calltree_consistent) {
					fprintf(F, "\n%s    recursion depth %d", prefix, get_irg_recursion_depth(get_entity_irg(ent)));
					fprintf(F, "\n%s    loop depth      %d", prefix, get_irg_loop_depth(get_entity_irg(ent)));
				}
#endif
			} else {
				fprintf(F, "\n%s  irg = NULL", prefix);
			}
		}
		fprintf(F, "\n");
	}

	if (get_trouts_state()) {
		fprintf(F, "%s  Entity outs:\n", prefix);
		dump_node_list(F, (firm_kind *)ent, prefix, (int(*)(firm_kind *))get_entity_n_accesses,
			(ir_node *(*)(firm_kind *, int))get_entity_access, "Accesses");
		dump_node_list(F, (firm_kind *)ent, prefix, (int(*)(firm_kind *))get_entity_n_references,
			(ir_node *(*)(firm_kind *, int))get_entity_reference, "References");
	}

	if (verbosity & dump_verbosity_accessStats) {
#ifdef EXTENDED_ACCESS_STATS
		int n_acc = get_entity_n_accesses(ent);
		int max_depth = 0;
		int max_L_freq = -1;
		int max_S_freq = -1;
		int max_LA_freq = -1;
		int max_SA_freq = -1;
		int *L_freq;
		int *S_freq;
		int *LA_freq;
		int *SA_freq;

		/* Find maximal depth */
		for (i = 0; i < n_acc; ++i) {
			ir_node *acc = get_entity_access(ent, i);
			int depth = get_weighted_loop_depth(acc);
			max_depth = (depth > max_depth) ? depth : max_depth ;
		}

		L_freq = xcalloc(4 * max_depth, sizeof(L_freq[0]));

		S_freq  = L_freq + 1*max_depth;
		LA_freq = L_freq + 2*max_depth;
		SA_freq = L_freq + 3*max_depth;

		for (i = 0; i < n_acc; ++i) {
			ir_node *acc = get_entity_access(ent, i);
			int depth = get_weighted_loop_depth(acc);
			assert(depth < max_depth);
			if ((get_irn_op(acc) == op_Load) || (get_irn_op(acc) == op_Call)) {
				L_freq[depth]++;
				max_L_freq = (depth > max_L_freq) ? depth : max_L_freq;
				if (addr_is_alloc(acc)) {
					LA_freq[depth]++;
					max_LA_freq = (depth > max_LA_freq) ? depth : max_LA_freq;
				}
			} else if (get_irn_op(acc) == op_Store) {
				S_freq[depth]++;
				max_S_freq = (depth > max_S_freq) ? depth : max_S_freq;
				if (addr_is_alloc(acc)) {
					SA_freq[depth]++;
					max_SA_freq = (depth > max_SA_freq) ? depth : max_SA_freq;
				}
			} else {
				assert(0);
			}
		}

		if (max_L_freq >= 0) {
			char comma = ':';

			fprintf(F, "%s  Load  Stats", prefix);
			for (i = 0; i <= max_L_freq; ++i) {
				if (L_freq[i])
					fprintf(F, "%c %d x  L%d", comma, L_freq[i], i);
				else
					fprintf(F, "         ");
				comma = ',';
			}
			fprintf(F, "\n");
		}
		if (max_LA_freq >= 0) {
			//fprintf(F, "%s  LoadA Stats", prefix);
			char comma = ':';
			for (i = 0; i <= max_LA_freq; ++i) {
				//if (LA_freq[i])
				//fprintf(F, "%c %d x LA%d", comma, LA_freq[i], i);
				//else
				//fprintf(F, "         ");
				comma = ',';
			}
			fprintf(F, "\n");
		}
		if (max_S_freq >= 0) {
			char comma = ':';

			fprintf(F, "%s  Store Stats", prefix);
			for (i = 0; i <= max_S_freq; ++i) {
				if (S_freq[i])
					fprintf(F, "%c %d x  S%d", comma, S_freq[i], i);
				else
					fprintf(F, "         ");
				comma = ',';
			}
			fprintf(F, "\n");
		}
		if (max_SA_freq >= 0) {
			//fprintf(F, "%s  StoreAStats", prefix);
			char comma = ':';
			for (i = 0; i <= max_SA_freq; ++i) {
				//if (SA_freq[i])
				//fprintf(F, "%c %d x SA%d", comma, SA_freq[i], i);
				//else
				//fprintf(F, "         ");
				comma = ',';
			}
			fprintf(F, "\n");
		}

		/* free allocated space */
		free(L_freq);
#endif
		if (get_trouts_state() != outs_none) {
#ifdef INTERPROCEDURAL_VIEW
			if (is_Method_type(get_entity_type(ent))) {
				fprintf(F, "%s  Estimated #Calls:    %lf\n", prefix, get_entity_estimated_n_calls(ent));
				fprintf(F, "%s  Estimated #dynCalls: %lf\n", prefix, get_entity_estimated_n_calls(ent));
			} else {
				fprintf(F, "%s  Estimated #Loads:  %lf\n", prefix, get_entity_estimated_n_loads(ent));
				fprintf(F, "%s  Estimated #Stores: %lf\n", prefix, get_entity_estimated_n_stores(ent));
			}
#endif
		}
	}
}

void    dump_entity_to_file (FILE *F, ir_entity *ent, unsigned verbosity) {
	dump_entity_to_file_prefix (F, ent, "", verbosity);
	fprintf(F, "\n");
}

void dump_entity(ir_entity *ent) {
  dump_entity_to_file(stdout, ent, dump_verbosity_max);
}

void dump_entitycsv_to_file_prefix(FILE *F, ir_entity *ent, char *prefix,
                                   unsigned verbosity, int *max_disp,
                                   int disp[], const char *comma)
{
	(void) verbosity;
	(void) max_disp;
	(void) disp;
	(void) comma;
#if 0   /* Outputs loop depth of all occurrences. */
	int n_acc = get_entity_n_accesses(ent);
	int max_L_freq = -1;
	int max_S_freq = -1;
	int max_LA_freq = -1;
	int max_SA_freq = -1;
	int *L_freq;
	int *S_freq;
	int *LA_freq;
	int *SA_freq;
	int i, max_depth = 0;

	/* Find maximal depth */
	for (i = 0; i < n_acc; ++i) {
		ir_node *acc = get_entity_access(ent, i);
		int depth = get_weighted_loop_depth(acc);
		max_depth = (depth > max_depth) ? depth : max_depth ;
	}

	L_freq = xcalloc(4 * (max_depth+1), sizeof(L_freq[0]));

	S_freq  = L_freq + 1*max_depth;
	LA_freq = L_freq + 2*max_depth;
	SA_freq = L_freq + 3*max_depth;

	for (i = 0; i < n_acc; ++i) {
		ir_node *acc = get_entity_access(ent, i);
		int depth = get_weighted_loop_depth(acc);
		assert(depth <= max_depth);
		if ((get_irn_op(acc) == op_Load) || (get_irn_op(acc) == op_Call)) {
			L_freq[depth]++;
			max_L_freq = (depth > max_L_freq) ? depth : max_L_freq;
			if (addr_is_alloc(acc)) {
				LA_freq[depth]++;
				max_LA_freq = (depth > max_LA_freq) ? depth : max_LA_freq;
			}
			if (get_entity_allocation(ent) == allocation_static) {
				disp[depth]++;
				*max_disp = (depth > *max_disp) ? depth : *max_disp;
			}
		} else if (get_irn_op(acc) == op_Store) {
			S_freq[depth]++;
			max_S_freq = (depth > max_S_freq) ? depth : max_S_freq;
			if (addr_is_alloc(acc)) {
				SA_freq[depth]++;
				max_SA_freq = (depth > max_SA_freq) ? depth : max_SA_freq;
			}
			if (get_entity_allocation(ent) == allocation_static) {
				assert(0);
			}
		} else {
			assert(0);
		}
	}

	if (get_entity_allocation(ent) != allocation_static) {

		fprintf(F, "%s_%s", get_type_name(get_entity_owner(ent)), get_entity_name(ent));

		if (max_L_freq >= 0) {
			fprintf(F, "%s Load", comma);
			for (i = 0; i <= max_L_freq; ++i) {
				fprintf(F, "%s %d", comma, L_freq[i]);
			}
		}
		if (max_S_freq >= 0) {
			if (max_L_freq >= 0)    fprintf(F, "\n%s_%s", get_type_name(get_entity_owner(ent)), get_entity_name(ent));
			fprintf(F, "%s Store", comma);
			for (i = 0; i <= max_S_freq; ++i) {
				fprintf(F, "%s %d", comma, S_freq[i]);
			}
		}
		fprintf(F, "\n");
	}
	free(L_freq);
#endif

	if (get_entity_allocation(ent) != allocation_static) {
		if (is_Method_type(get_entity_type(ent))) return;

		/* Output the entity name. */
		fprintf(F, "%s%-40s ", prefix, get_entity_ld_name(ent));

#ifdef INTERPROCEDURAL_VIEW
		if (get_trouts_state() != outs_none) {
			if (is_Method_type(get_entity_type(ent))) {
				//fprintf(F, "%s  Estimated #Calls:    %lf\n", prefix, get_entity_estimated_n_calls(ent));
				//fprintf(F, "%s  Estimated #dynCalls: %lf\n", prefix, get_entity_estimated_n_calls(ent));
			} else {
				fprintf(F, "%6.2lf ", get_entity_estimated_n_loads(ent));
				fprintf(F, "%6.2lf", get_entity_estimated_n_stores(ent));
			}
		}
#endif

		fprintf(F, "\n");
	}
}

#ifdef INTERPROCEDURAL_VIEW
/* A fast hack to dump a CSV-file. */
void dump_typecsv_to_file(FILE *F, ir_type *tp, dump_verbosity verbosity, const char *comma) {
	int i;
	char buf[1024 + 10];
	(void) comma;

	if (!is_Class_type(tp)) return;   // we also want array types. Stupid, these are classes in java.

	if (verbosity & dump_verbosity_accessStats) {

#if 0
		/* Outputs loop depth of all occurrences. */
		int max_freq = -1;
		int max_disp = -1;
		int *freq, *disp; /* Accumulated accesses to static members: dispatch table. */
		int n_all = get_type_n_allocs(tp);
		int max_depth = 0;
		/* Find maximal depth */
		for (i = 0; i < n_all; ++i) {
			ir_node *all = get_type_alloc(tp, i);
			int depth = get_weighted_loop_depth(all);
			max_depth = (depth > max_depth) ? depth : max_depth ;
		}

		freq = xcalloc(2 * (max_depth+1), sizeof(freq[0]));

		disp = freq + max_depth;

		for (i = 0; i < n_all; ++i) {
			ir_node *all = get_type_alloc(tp, i);
			int depth = get_weighted_loop_depth(all);
			assert(depth <= max_depth);
			freq[depth]++;
			max_freq = (depth > max_freq) ? depth : max_freq;
			assert(get_irn_op(all) == op_Alloc);
		}

		fprintf(F, "%s ", get_type_name(tp));
		fprintf(F, "%s Alloc ", comma);

		if (max_freq >= 0) {
			for (i = 0; i <= max_freq; ++i) {
				fprintf(F, "%s %d", comma, freq[i]);
			}
		}
		fprintf(F, "\n");

		for (i = 0; i < get_class_n_members(tp); ++i) {
			ir_entity *mem = get_class_member(tp, i);
			if (((verbosity & dump_verbosity_methods) &&  is_Method_type(get_entity_type(mem))) ||
				((verbosity & dump_verbosity_fields)  && !is_Method_type(get_entity_type(mem)))   ) {
				if (!((verbosity & dump_verbosity_nostatic) && (get_entity_allocation(mem) == allocation_static))) {
					dump_entitycsv_to_file_prefix(F, mem, "    ", verbosity, &max_disp, disp, comma);
				}
			}
		}

		if (max_disp >= 0) {
			fprintf(F, "%s__disp_tab%s Load", get_type_name(tp), comma);
			for (i = 0; i <= max_disp; ++i) {
				fprintf(F, "%s %d", comma, disp[i]);
			}
			fprintf(F, "\n");
		}

		/* free allocated space */
		free(freq);
#endif

#define DISP_TAB_SUFFIX "__disp_tab"
		if (get_trouts_state() != outs_none) {
			assert(strlen(get_type_name(tp)) < 1024);
			fprintf(F, "%-44s %6.2lf  -1.00\n", get_type_name(tp), get_type_estimated_n_instances(tp));
			sprintf(buf, "%s%s", get_type_name(tp), DISP_TAB_SUFFIX);
			fprintf(F, "%-44s %6.2lf   0.00\n", buf, get_class_estimated_n_dyncalls(tp));
		}

		for (i = 0; i < get_class_n_members(tp); ++i) {
			ir_entity *mem = get_class_member(tp, i);
			if (((verbosity & dump_verbosity_methods) &&  is_Method_type(get_entity_type(mem))) ||
				((verbosity & dump_verbosity_fields)  && !is_Method_type(get_entity_type(mem)))   ) {
				if (!((verbosity & dump_verbosity_nostatic) && (get_entity_allocation(mem) == allocation_static))) {
					dump_entitycsv_to_file_prefix(F, mem, "    ", verbosity, NULL, 0, 0);
				}
			}
		}
	}
}
#endif

void dump_type_to_file(FILE *F, ir_type *tp, dump_verbosity verbosity) {
	int i;

	if ((is_Class_type(tp))       && (verbosity & dump_verbosity_noClassTypes)) return;
	if ((is_Struct_type(tp))      && (verbosity & dump_verbosity_noStructTypes)) return;
	if ((is_Union_type(tp))       && (verbosity & dump_verbosity_noUnionTypes)) return;
	if ((is_Array_type(tp))       && (verbosity & dump_verbosity_noArrayTypes)) return;
	if ((is_Pointer_type(tp))     && (verbosity & dump_verbosity_noPointerTypes)) return;
	if ((is_Method_type(tp))      && (verbosity & dump_verbosity_noMethodTypes)) return;
	if ((is_Primitive_type(tp))   && (verbosity & dump_verbosity_noPrimitiveTypes)) return;
	if ((is_Enumeration_type(tp)) && (verbosity & dump_verbosity_noEnumerationTypes)) return;

	fprintf(F, "%s type %s (%ld)", get_tpop_name(get_type_tpop(tp)), get_type_name(tp), get_type_nr(tp));
	if (verbosity & dump_verbosity_onlynames) { fprintf(F, "\n"); return; }

	switch (get_type_tpop_code(tp)) {

	case tpo_class:
		if ((verbosity & dump_verbosity_methods) || (verbosity & dump_verbosity_fields)) {
			fprintf(F, "\n  members: \n");
		}
		for (i = 0; i < get_class_n_members(tp); ++i) {
			ir_entity *mem = get_class_member(tp, i);
			if (((verbosity & dump_verbosity_methods) &&  is_Method_type(get_entity_type(mem))) ||
				((verbosity & dump_verbosity_fields)  && !is_Method_type(get_entity_type(mem)))   ) {
				if (!((verbosity & dump_verbosity_nostatic) && (get_entity_allocation(mem) == allocation_static))) {
					dump_entity_to_file_prefix(F, mem, "    ", verbosity);
				}
			}
		}
		if (verbosity & dump_verbosity_typeattrs) {
			fprintf(F, "  supertypes: ");
			for (i = 0; i < get_class_n_supertypes(tp); ++i) {
				ir_type *stp = get_class_supertype(tp, i);
				fprintf(F, "\n    %d %s", i, get_type_name(stp));
			}
			fprintf(F, "\n  subtypes: ");
			for (i = 0; i < get_class_n_subtypes(tp); ++i) {
				ir_type *stp = get_class_subtype(tp, i);
				fprintf(F, "\n    %d %s", i, get_type_name(stp));
			}

			if (get_irp_inh_transitive_closure_state() != inh_transitive_closure_none) {
				ir_type *stp;
				fprintf(F, "\n  transitive supertypes: ");
				for (stp = get_class_trans_supertype_first(tp);
				stp;
				stp = get_class_trans_supertype_next(tp)) {
					fprintf(F, "\n    %s", get_type_name(stp));
				}
				fprintf(F, "\n  transitive subtypes: ");
				for (stp = get_class_trans_subtype_first(tp);
				stp;
				stp = get_class_trans_subtype_next(tp)) {
					fprintf(F, "\n    %s", get_type_name(stp));
				}
			}

			fprintf(F, "\n  peculiarity: %s\n", get_peculiarity_name(get_class_peculiarity(tp)));
			fprintf(F, "\n  flags:       ");
			if (is_class_final(tp))
				fprintf(F, "final, ");
			if (is_class_interface(tp))
				fprintf(F, "interface, ");
			if (is_class_abstract(tp))
				fprintf(F, "abstract, ");
			fprintf(F, "\n");
		}
		break;

	case tpo_union:
	case tpo_struct:
		if (verbosity & dump_verbosity_fields) fprintf(F, "\n  members: ");
		for (i = 0; i < get_compound_n_members(tp); ++i) {
			ir_entity *mem = get_compound_member(tp, i);
			if (verbosity & dump_verbosity_fields) {
				dump_entity_to_file_prefix(F, mem, "    ", verbosity);
			}
		}
		break;

	case tpo_array:
		if (verbosity & dump_verbosity_typeattrs) {
			int i, n_dim;
			ir_type *elem_tp = get_array_element_type(tp);

			fprintf(F, "\n  array ");

			n_dim = get_array_n_dimensions(tp);
			for (i = 0; i < n_dim; ++i) {
				ir_node *lower, *upper;

				lower = get_array_lower_bound(tp, i);
				upper = get_array_upper_bound(tp, i);

				fprintf(F, "[");

				if (get_irn_op(lower) == op_Const)
					fprintf(F, "%ld .. ", get_tarval_long(get_Const_tarval(lower)));
				else {
					dump_node_opcode(F, lower);
					fprintf(F, " %ld .. ", get_irn_node_nr(lower));
				}

				if (get_irn_op(upper) == op_Const)
					fprintf(F, "%ld]", get_tarval_long(get_Const_tarval(lower)));
				else {
					dump_node_opcode(F, upper);
					fprintf(F, " %ld]", get_irn_node_nr(upper));
				}
			}
			fprintf(F, " of <%s (%ld)>", get_type_name(elem_tp), get_type_nr(elem_tp));

			fprintf(F, "\n  order: ");
			for (i = 0; i < n_dim; ++i)
				fprintf(F, "<%d>", get_array_order(tp, i));

			fprintf(F, "\n");

			if (verbosity & dump_verbosity_fields) {
				dump_entity_to_file_prefix(F, get_array_element_entity(tp),
					"    ", verbosity);
			}
		}
		break;

	case tpo_pointer:
		if (verbosity & dump_verbosity_typeattrs) {
			ir_type *tt = get_pointer_points_to_type(tp);
			fprintf(F, "\n  points to %s (%ld)\n", get_type_name(tt), get_type_nr(tt));
		}
		break;

	case tpo_method:
		if (verbosity & dump_verbosity_typeattrs) {
			fprintf(F, "\n  variadicity: %s", get_variadicity_name(get_method_variadicity(tp)));
			fprintf(F, "\n  return types: %d", get_method_n_ress(tp));
			for (i = 0; i < get_method_n_ress(tp); ++i) {
				ir_type *rtp = get_method_res_type(tp, i);
				fprintf(F, "\n    %s", get_type_name(rtp));
			}

			fprintf(F, "\n  parameter types: %d", get_method_n_params(tp));
			for (i = 0; i < get_method_n_params(tp); ++i) {
				ir_type *ptp = get_method_param_type(tp, i);
				fprintf(F, "\n    %s", get_type_name(ptp));
			}
			if (get_method_variadicity(tp)) {
				fprintf(F, "\n    ...");
			}
			fprintf(F, "\n");
		}
		break;

	case tpo_primitive:
		if (verbosity & dump_verbosity_typeattrs) {
			ir_type *base_tp = get_primitive_base_type(tp);
			if (base_tp != NULL)
				fprintf(F, "\n  base type: %s (%ld)", get_type_name(tp), get_type_nr(tp));
			fprintf(F, "\n");
		}
		break;

	case tpo_id:
	case tpo_none:
	case tpo_unknown:
		fprintf(F, "\n");
		break;

	default:
		if (verbosity & dump_verbosity_typeattrs) {
			fprintf(F, ": details not implemented\n");
		}
	}

	fprintf(F, "  visibility: %s,\n", get_visibility_name(get_type_visibility(tp)));
	fprintf(F, "  state:      %s,\n", get_type_state_name(get_type_state(tp)));
	fprintf(F, "  size:       %2d Bits,\n",  get_type_size_bits(tp));
	fprintf(F, "  alignment:  %2d Bits,\n",  get_type_alignment_bits(tp));
	if (is_atomic_type(tp) || is_Method_type(tp))
		fprintf(F, "  mode:       %s,\n",  get_mode_name(get_type_mode(tp)));

	if (get_trouts_state()) {
		fprintf(F, "\n  Type outs:\n");
		dump_node_list(F, (firm_kind *)tp, "  ", (int(*)(firm_kind *))get_type_n_allocs,
			(ir_node *(*)(firm_kind *, int))get_type_alloc, "Allocations");
		dump_node_list(F, (firm_kind *)tp, "  ", (int(*)(firm_kind *))get_type_n_casts,
			(ir_node *(*)(firm_kind *, int))get_type_cast, "Casts");
		dump_type_list(F, tp, "  ", get_type_n_pointertypes_to, get_type_pointertype_to, "PointerTpsTo");
	}


	if (verbosity & dump_verbosity_accessStats) {
#if 0
		int n_all = get_type_n_allocs(tp);
		int max_depth = 0;
		int max_freq = -1;
		int *freq;

		/* Find maximal depth */
		for (i = 0; i < n_all; ++i) {
			ir_node *all = get_type_alloc(tp, i);
			int depth = get_weighted_loop_depth(all);
			max_depth = (depth > max_depth) ? depth : max_depth ;
		}

		freq = xcalloc(max_depth+1, sizeof(freq[0]));

		for (i = 0; i < n_all; ++i) {
			ir_node *all = get_type_alloc(tp, i);
			int depth = get_weighted_loop_depth(all);
			assert(depth <= max_depth);
			freq[depth]++;
			max_freq = (depth > max_freq) ? depth : max_freq;
			assert(get_irn_op(all) == op_Alloc);
		}

		if (max_freq >= 0) {
			char comma = ':';

			fprintf(F, "  Alloc Stats");
			for (i = 0; i <= max_freq; ++i) {
				fprintf(F, "%c %d x A%d", comma, freq[i], i);
				comma = ',';
			}
			fprintf(F, "\n");
		}

		free(freq);
#endif
#ifdef INTERPROCEDURAL_VIEW
		if (get_trouts_state() != outs_none) {
			fprintf(F, "  Estimated #Instances: %lf\n", get_type_estimated_n_instances(tp));
			if (is_Class_type(tp) && (get_irp_typeinfo_state() != ir_typeinfo_none)) {
				fprintf(F, "  Estimated #dyn Calls: %lf\n", get_class_estimated_n_dyncalls(tp));
				fprintf(F, "  Estimated #Upcasts:   %lf (#CastOps: %d)\n", get_class_estimated_n_upcasts(tp), get_class_n_upcasts(tp));
				fprintf(F, "  Estimated #Downcasts: %lf (#CastOps: %d)\n", get_class_estimated_n_downcasts(tp), get_class_n_downcasts(tp));
				assert(get_class_n_upcasts(tp) + get_class_n_downcasts(tp) == get_type_n_casts(tp));
			}
		}
#endif

	}

	fprintf(F, "\n\n");
}

void dump_type(ir_type *tp) {
	dump_type_to_file (stdout, tp, dump_verbosity_max);
}


void dump_types_as_text(unsigned verbosity, const char *suffix) {
	const char *basename;
	FILE *F, *CSV = NULL;
	int i, n_types = get_irp_n_types();

	basename = irp_prog_name_is_set() ? get_irp_prog_name() : "TextTypes";
	F = text_open(basename, suffix, "-types", ".txt");

	if (verbosity & dump_verbosity_csv) {
		CSV = text_open(basename, suffix, "-types", ".csv");
		//fprintf(CSV, "Class, Field, Operation, L0, L1, L2, L3\n");
	}

	for (i = 0; i < n_types; ++i) {
		ir_type *t = get_irp_type(i);

		//if (is_jack_rts_class(t)) continue;

		dump_type_to_file(F, t, verbosity);
#ifdef INTERPROCEDURAL_VIEW
		if (CSV) {
			dump_typecsv_to_file(CSV, t, verbosity, "");
		}
#endif
	}

	fclose(F);
	if (CSV) fclose(CSV);
}


void dump_globals_as_text(unsigned verbosity, const char *suffix) {
	const char *basename;
	FILE *F, *CSV = NULL;
	ir_type *g = get_glob_type();
	int i, n_mems = get_class_n_members(g);

	basename = irp_prog_name_is_set() ? get_irp_prog_name() : "TextGlobals";
	F = text_open (basename, suffix, "-globals", ".txt");

	if (verbosity & dump_verbosity_csv) {
		CSV = text_open (basename, suffix, "-types", ".csv");
		//fprintf(CSV, "Class, Field, Operation, L0, L1, L2, L3\n");
	}

	for (i = 0; i < n_mems; ++i) {
		ir_entity *e = get_class_member(g, i);

		dump_entity_to_file(F, e, verbosity);
		if (CSV) {
			//dump_entitycsv_to_file_prefix(CSV, e, "", verbosity, ""???);
		}
	}

	fclose (F);
	if (CSV) fclose (CSV);
}
