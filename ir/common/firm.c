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
 * @brief     Central firm functionality.
 * @author    Martin Trapp, Christian Schaefer, Goetz Lindenmaier
 * @version   $Id$
 */
#include "config.h"

#ifdef HAVE_FIRM_REVISION_H
# include "firm_revision.h"
#endif

#include <string.h>
#include <stdio.h>

#include "lc_opts.h"

#include "ident_t.h"
#include "firm.h"
#include "irflag_t.h"
#include "tv_t.h"
#include "tpop_t.h"
#include "irprog_t.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "ircons_t.h"
#include "irgraph_t.h"
#include "type_t.h"
#include "entity_t.h"
#include "firmstat.h"
#include "irarch.h"
#include "irhooks.h"
#include "iredges_t.h"
#include "irmemory_t.h"
#include "opt_init.h"
#include "debugger.h"
#include "be_t.h"
#include "irtools.h"

/* returns the firm root */
lc_opt_entry_t *firm_opt_get_root(void)
{
	static lc_opt_entry_t *grp = NULL;
	if (!grp)
		grp = lc_opt_get_grp(lc_opt_root_grp(), "firm");
	return grp;
}

void ir_init(const firm_parameter_t *param)
{
	firm_parameter_t def_params;
	unsigned int     size;

	/* for historical reasons be_init must be run first */
	firm_be_init();

	memset(&def_params, 0, sizeof(def_params));

	if (param) {
		/* check for reasonable size */
		assert(param->size <= sizeof(def_params) && (param->size & 3) == 0 &&
				"parameter struct not initialized ???");
		size = sizeof(def_params);
		if (param->size < size)
			size = param->size;

		memcpy(&def_params, param, size);
	}

	/* initialize firm flags */
	firm_init_flags();
	/* initialize all ident stuff */
	init_ident();
	/* enhanced statistics, need idents and hooks */
	if (def_params.enable_statistics != 0)
		firm_init_stat(def_params.enable_statistics);
	/* Edges need hooks. */
	init_edges();
	/* create the type kinds. */
	init_tpop();
	/* create an obstack and put all tarvals in a pdeq */
	init_tarval_1(0l, /* support_quad_precision */0);
	/* Builds a basic program representation, so modes can be added. */
	init_irprog_1();
	/* initialize all modes an ir node can consist of */
	init_mode();
	/* initialize tarvals, and floating point arithmetic */
	init_tarval_2();
	/* init graph construction */
	firm_init_irgraph();
	/* kind of obstack initialization */
	firm_init_mangle();
	/* initialize all op codes an irnode can consist of */
	init_op();
	/* called once for each run of this library */
	if (def_params.initialize_local_func != NULL)
		ir_set_uninitialized_local_variable_func(
				def_params.initialize_local_func);
	/* initialize reassociation */
	firm_init_reassociation();
	/* initialize function call optimization */
	firm_init_funccalls();
	/* initialize function inlining */
	firm_init_inline();
	/* initialize scalar replacement */
	firm_init_scalar_replace();
	/* Builds a construct allowing to access all information to be constructed
	   later. */
	init_irprog_2();
	/* Initialize the type module and construct some idents needed. */
	ir_init_type();
	/* initialize the entity module */
	ir_init_entity();
	/* class cast optimization */
	firm_init_class_casts_opt();
	/* memory disambiguation */
	firm_init_memory_disambiguator();
	firm_init_loop_opt();

	/* Init architecture dependent optimizations. */
	arch_dep_set_opts(arch_dep_none);

	init_irnode();

#ifdef DEBUG_libfirm
	/* integrated debugger extension */
	firm_init_debugger();
#endif
}

void ir_finish(void)
{
	size_t i;

	/* must iterate backwards here */
	for (i = get_irp_n_irgs(); i > 0;)
		free_ir_graph(get_irp_irg(--i));

	free_type_entities(get_glob_type());
	/* must iterate backwards here */
	for (i = get_irp_n_types(); i > 0;)
		free_type_entities(get_irp_type(--i));

	for (i = get_irp_n_types(); i > 0;)
		free_type(get_irp_type(--i));

	free_ir_prog();

	ir_finish_entity();
	ir_finish_type();

	finish_tarval();
	finish_mode();
	finish_tpop();
	finish_ident();

	firm_be_finish();
}

unsigned ir_get_version_major(void)
{
	return libfirm_VERSION_MAJOR;
}

unsigned ir_get_version_minor(void)
{
	return libfirm_VERSION_MINOR;
}

const char *ir_get_version_revision(void)
{
#ifdef libfirm_VERSION_REVISION
	return libfirm_VERSION_REVISION;
#else
	return "";
#endif
}

const char *ir_get_version_build(void)
{
	return "";
}
