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
 * @brief    Implements the Firm interface to debug information.
 * @author   Goetz Lindenmaier, Michael Beck
 * @date     2001
 * @version  $Id$
 */
#include "config.h"

#include "dbginfo_t.h"
#include "irnode_t.h"
#include "type_t.h"
#include "entity_t.h"

merge_pair_func *__dbg_info_merge_pair = default_dbg_info_merge_pair;
merge_sets_func *__dbg_info_merge_sets = default_dbg_info_merge_sets;

void dbg_init(merge_pair_func *mpf, merge_sets_func *msf)
{
	__dbg_info_merge_pair = mpf ? mpf : default_dbg_info_merge_pair;
	__dbg_info_merge_sets = msf ? msf : default_dbg_info_merge_sets;
}

/*
 * Converts a debug_action into a string.
 */
const char *dbg_action_2_str(dbg_action a)
{
#define CASE(a) case a: return #a

	switch (a) {
	CASE(dbg_error);
	CASE(dbg_opt_ssa);
	CASE(dbg_opt_auxnode);
	CASE(dbg_const_eval);
	CASE(dbg_opt_cse);
	CASE(dbg_straightening);
	CASE(dbg_if_simplification);
	CASE(dbg_algebraic_simplification);
	CASE(dbg_write_after_write);
	CASE(dbg_write_after_read);
	CASE(dbg_read_after_write);
	CASE(dbg_read_after_read);
	CASE(dbg_read_a_const);
	CASE(dbg_rem_poly_call);
	CASE(dbg_dead_code);
	CASE(dbg_opt_confirm);
	CASE(dbg_gvn_pre);
	CASE(dbg_combo);
	CASE(dbg_jumpthreading);
	CASE(dbg_backend);
	default:
		if (a <= dbg_max)
			return "string conversion not implemented";
		else
			assert(!"Missing debug action in dbg_action_2_str()");
		return NULL;
	}
#undef CASE
}

void default_dbg_info_merge_pair(ir_node *nw, ir_node *old, dbg_action info)
{
	dbg_info *new_db = get_irn_dbg_info(nw);
	(void) info;
	if (new_db == NULL)
		set_irn_dbg_info(nw, get_irn_dbg_info(old));
}

void default_dbg_info_merge_sets(ir_node **new_nodes, int n_new_nodes,
                                 ir_node **old_nodes, int n_old_nodes,
                                 dbg_action info)
{
	(void) info;
	if (n_old_nodes == 1) {
		dbg_info *old_db = get_irn_dbg_info(old_nodes[0]);
		int i;

		for (i = 0; i < n_new_nodes; ++i)
			if (get_irn_dbg_info(new_nodes[i]) == NULL)
				set_irn_dbg_info(new_nodes[i], old_db);
	}
}

/** The debug info retriever function. */
static retrieve_dbg_func      retrieve_dbg      = NULL;
static retrieve_type_dbg_func retrieve_type_dbg = NULL;

void ir_set_debug_retrieve(retrieve_dbg_func func)
{
	retrieve_dbg = func;
}

const char *ir_retrieve_dbg_info(const dbg_info *dbg, unsigned *line)
{
	if (retrieve_dbg)
		return retrieve_dbg(dbg, line);

	*line = 0;
	return NULL;
}

void ir_set_type_debug_retrieve(retrieve_type_dbg_func func)
{
	retrieve_type_dbg = func;
}

void ir_retrieve_type_dbg_info(char *buffer, size_t buffer_size,
                               const type_dbg_info *tdbgi)
{
	if (retrieve_type_dbg)
		retrieve_type_dbg(buffer, buffer_size, tdbgi);
	assert(buffer_size > 0);
	buffer[0] = 0;
}

void ir_dbg_info_snprint(char *buf, size_t bufsize, const dbg_info *dbg)
{
	unsigned    line;
	const char *source = ir_retrieve_dbg_info(dbg, &line);

	if (source == NULL) {
		assert(bufsize > 0);
		buf[0] = 0;
		return;
	}
	snprintf(buf, bufsize, "%s:%u", source, line);
}
