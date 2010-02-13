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
 * @author    Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Michael Beck
 * @version   $Id$
 */
#include "config.h"

#include "irgraph.h"
#include "irloop.h"
#include "tv.h"

/**
 * Ideally, this macro would check if size bytes could be read at
 * pointer p. No generic solution.
 */
#define POINTER_READ(p, size) (p)

/* returns the kind of the thing */
firm_kind get_kind(const void *firm_thing)
{
	return POINTER_READ(firm_thing, sizeof(firm_kind)) ? *(firm_kind *)firm_thing : k_BAD;
}  /* get_kind */

const char *print_firm_kind(void *firm_thing)
{
	if (! firm_thing)
		return "(NULL)";

	switch (*(firm_kind *)firm_thing) {
	case k_entity                 : return "k_entity";
	case k_type                   : return "k_type";
	case k_ir_graph               : return "k_ir_graph";
	case k_ir_node                : return "k_ir_node";
	case k_ir_mode                : return "k_ir_mode";
	case k_ir_op                  : return "k_ir_op";
	case k_tarval                 : return "k_tarval";
	case k_ir_loop                : return "k_ir_loop";
	case k_ir_compound_graph_path : return "k_ir_compound_graph_path";
	case k_ir_extblk              : return "k_ir_extblk";
	case k_ir_prog                : return "k_ir_prog";
	case k_ir_region              : return "k_ir_region";

	default: return "";
	}
}  /* print_firm_kind */

/*
 * identify a firm thing
 */
void firm_identify_thing(void *X)
{
	if (! X) {
		printf("(NULL)\n");
		return;
	}

	switch (get_kind(X)) {
	case k_BAD:
		printf("BAD: (%p)\n", X);
		break;
	case k_entity:
		printf("entity: %s: %ld (%p)\n", get_entity_name(X), get_entity_nr(X), X);
		break;
	case k_type: {
		char buf[256];
		ir_print_type(buf, sizeof(buf), X);
		printf("type: %s '%s': %ld (%p)\n", get_type_tpop_name(X), buf, get_type_nr(X), X);
		break;
	}
	case k_ir_graph:
		printf("graph: %s: %ld (%p)\n", get_entity_name(get_irg_entity(X)), get_irg_graph_nr(X), X);
		break;
	case k_ir_node:
		printf("irnode: %s %s %ld (%p)\n", get_irn_opname(X), get_mode_name(get_irn_mode(X)), get_irn_node_nr(X), X);
		break;
	case k_ir_mode:
		printf("mode %s: (%p)\n", get_mode_name(X), X);
		break;
	case k_ir_op:
		printf("op %s: (%p)\n", get_op_name(X), X);
		break;
	case k_tarval:
		printf("tarval : "); tarval_printf(X); printf(" (%p)\n", X);
		break;
	case k_ir_loop:
		printf("loop: with depth %d: (%p)\n", get_loop_depth(X), X);
		break;
	case k_ir_compound_graph_path:
		printf("compound_graph_path: (%p)\n", X);
		break;
	case k_ir_extblk:
		printf("extended block: (%p)\n", X);
		break;
	case k_ir_prog:
		printf("irp: (%p)\n", X);
		break;
	case k_ir_region:
		printf("region: (%p)\n", X);
		break;
	default:
		printf("Cannot identify thing at (%p).\n", X);
	}
}  /* firm_identify_thing */
