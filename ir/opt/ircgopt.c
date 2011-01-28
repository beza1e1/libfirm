/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
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
 * @brief    Removal of unreachable methods.
 * @author   Hubert Schmid
 * @date     09.06.2002
 * @version  $Id$
 */

/*
 * Entfernen von nicht erreichbaren (aufrufbaren) Methoden. Die Menge
 * der nicht erreichbaren Methoden wird aus der Abschätzung der
 * Aufrufrelation bestimmt.
 */
#include "config.h"

#include "ircgopt.h"

#include "debug.h"
#include "array.h"
#include "irprog.h"
#include "irgwalk.h"
#include "irloop_t.h"
#include "irflag_t.h"
#include "ircons.h"
#include "cgana.h"
#include "irtools.h"
#include "irpass.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg);

/**
 * Walker: adds Call operations to a head's link list.
 */
static void collect_call(ir_node *node, void *env)
{
	ir_node *head = (ir_node*)env;

	if (is_Call(node)) {
		set_irn_link(node, get_irn_link(head));
		set_irn_link(head, node);
	}
}

/* garbage collect methods: mark and remove */
void gc_irgs(size_t n_keep, ir_entity ** keep_arr)
{
	void * MARK = &MARK; /* @@@ gefaehrlich!!! Aber wir markieren hoechstens zu viele ... */
	int i;

	FIRM_DBG_REGISTER(dbg, "firm.opt.cgopt");

	if (n_keep >= get_irp_n_irgs()) {
		/* Shortcut. Obviously we have to keep all methods. */
		return;
	}

	DB((dbg, LEVEL_1, "dead method elimination\n"));

	/* Mark entities that are alive.  */
	if (n_keep > 0) {
		ir_entity **marked = NEW_ARR_F(ir_entity *, n_keep);
		size_t    idx;

		for (idx = 0; idx < n_keep; ++idx) {
			marked[idx] = keep_arr[idx];
			set_entity_link(marked[idx], MARK);
			DB((dbg, LEVEL_1, "  method %+F kept alive.\n", marked[idx]));
		}

		for (idx = 0; idx < ARR_LEN(marked); ++idx) {
			ir_graph *irg = get_entity_irg(marked[idx]);
			ir_node *node;

			if (irg == NULL)
				continue;

			node = get_irg_end(irg);

			/* collect calls */
			ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
			irg_walk_graph(irg, firm_clear_link, collect_call, node);

			/* iterate calls */
			for (node = (ir_node*)get_irn_link(node); node != NULL;
			     node = (ir_node*)get_irn_link(node)) {
				int i;
				assert(is_Call(node));

				for (i = get_Call_n_callees(node) - 1; i >= 0; --i) {
					ir_entity *ent = get_Call_callee(node, i);

					if (get_entity_irg(ent) && get_entity_link(ent) != MARK) {
						set_entity_link(ent, MARK);
						ARR_APP1(ir_entity *, marked, ent);

						DB((dbg, LEVEL_1, "  method %+F can be called from Call %+F: kept alive.\n",
							ent, node));
					}
				}
			}
			ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
		}
		DEL_ARR_F(marked);
	}

	/* clean */
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_graph  *irg = get_irp_irg(i);
		ir_entity *ent = get_irg_entity(irg);

		if (get_entity_link(ent) == MARK)
			continue;

		DB((dbg, LEVEL_1, "  freeing method %+F\n", ent));
		remove_irp_irg(irg);

		free_entity(ent);
	}
}

/**
 * Wrapper for running gc_irgs() as an ir_prog pass.
 */
static void pass_wrapper(void)
{
    ir_entity **keep_methods;
    size_t    arr_len;

    /* Analysis that finds the free methods,
       i.e. methods that are dereferenced.
       Optimizes polymorphic calls :-). */
    arr_len = cgana(&keep_methods);

    /* Remove methods that are never called. */
    gc_irgs(arr_len, keep_methods);

    xfree(keep_methods);
}

ir_prog_pass_t *gc_irgs_pass(const char *name)
{
	return def_prog_pass(name ? name : "cgana", pass_wrapper);
}
