/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
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
 * @brief     Manager for optimization passes.
 * @author    Michael Beck
 * @version   $Id$
 */
#include "config.h"

#include <string.h>
#include "adt/list.h"
#include "irpass_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "irdump.h"
#include "irverify.h"
#include "xmalloc.h"

typedef void (*void_pass_func_irg)(ir_graph *irg);
typedef int (*int_pass_func_irg)(ir_graph *irg);
typedef void (*void_pass_func)(void);

/*Add a graph pass to a graph pass manager. */
void ir_graph_pass_mgr_add(ir_graph_pass_manager_t *mgr, ir_graph_pass_t *pass)
{
	list_add_tail(&pass->list, &mgr->passes);
	++mgr->n_passes;
	if (pass->add_to_mgr)
		pass->add_to_mgr(pass->context);
}

/* Add an irprog pass to an irprog pass manager. */
void ir_prog_pass_mgr_add(ir_prog_pass_manager_t *mgr, ir_prog_pass_t *pass)
{
	list_add_tail(&pass->list, &mgr->passes);
	++mgr->n_passes;
	if (pass->add_to_mgr)
		pass->add_to_mgr(pass->context);
}

/**
 * wrapper for running a graph pass manager as a pass on an irprog
 * pass manager.
 */
static int run_wrapper(ir_prog *prog, void *ctx)
{
	ir_graph_pass_manager_t *mgr = (ir_graph_pass_manager_t*)ctx;

	(void)prog;
	return ir_graph_pass_mgr_run(mgr);
}

/* Ensure that no verifier is run an ir_prog pass. */
int ir_prog_no_verify(ir_prog *prog, void *ctx)
{
	(void)prog;
	(void)ctx;
	return 0;
}

/* Ensure that no dumper is run from an ir_prog pass. */
void ir_prog_no_dump(ir_prog *prog, void *ctx, unsigned idx)
{
	(void)prog;
	(void)ctx;
	(void)idx;
}

/**
 * Term wrapper for a wrapped ir_graph pass manager.
 */
static void term_wrapper(void *context)
{
	ir_graph_pass_manager_t *mgr = (ir_graph_pass_manager_t*)context;
	term_graph_pass_mgr(mgr);
}

/**
 * Create a wrapper ir_prog pass for an ir_graph manager.
 */
static ir_prog_pass_t *create_wrapper_pass(ir_graph_pass_manager_t *graph_mgr)
{
	/* create a wrapper pass */
	ir_prog_pass_t *pass = XMALLOCZ(ir_prog_pass_t);

	pass->kind          = k_ir_prog_pass;
	pass->run_on_irprog = run_wrapper;
	pass->context       = graph_mgr;
	pass->name          = graph_mgr->name;

	/* do not verify nor dump: this is handled by the graph manager */
	pass->verify_irprog = ir_prog_no_verify;
	pass->dump_irprog   = ir_prog_no_dump;
	pass->is_wrapper    = 1;

	pass->add_to_mgr   = NULL;
	pass->rem_from_mgr = term_wrapper;

	return pass;
}

/* Add an ir_graph_pass as a pass to an ir_prog pass manager. */
void ir_prog_pass_mgr_add_graph_pass(
	ir_prog_pass_manager_t *mgr, ir_graph_pass_t *pass)
{
	ir_graph_pass_manager_t *graph_mgr;
	ir_prog_pass_t          *wrapper;

	/* check if the last pass is a graph_pass wrapper */
	if (! list_empty(&mgr->passes)) {
		wrapper = list_entry(mgr->passes.prev, ir_prog_pass_t, list);
		if (wrapper->is_wrapper) {
			graph_mgr = (ir_graph_pass_manager_t*)wrapper->context;

			ir_graph_pass_mgr_add(graph_mgr, pass);
			++mgr->n_passes;
			return;
		}
	}

	/* not found, create a new wrapper */
	graph_mgr = new_graph_pass_mgr(
		"graph_pass_wrapper", mgr->verify_all, mgr->dump_all);
	graph_mgr->run_idx = mgr->run_idx + mgr->n_passes;

	ir_graph_pass_mgr_add(graph_mgr, pass);

	wrapper = create_wrapper_pass(graph_mgr);
	ir_prog_pass_mgr_add(mgr, wrapper);
}

/* Add an ir_graph_pass_manager as a pass to an ir_prog pass manager. */
void ir_prog_pass_mgr_add_graph_mgr(
	ir_prog_pass_manager_t *mgr, ir_graph_pass_manager_t *graph_mgr)
{
	ir_prog_pass_t *pass = create_wrapper_pass(graph_mgr);

	if (mgr->dump_all)
		graph_mgr->dump_all = 1;
	if (mgr->verify_all)
		graph_mgr->verify_all = 1;
	graph_mgr->run_idx = mgr->n_passes;

	ir_prog_pass_mgr_add(mgr, pass);
}

static void create_suffix(char *suffix, size_t n, const char *pass_name)
{
	snprintf(suffix, n, "%s.svg", pass_name);
}

/* Run all passes of an ir_graph pass manager. */
int ir_graph_pass_mgr_run(ir_graph_pass_manager_t *mgr)
{
	ir_graph_pass_t *pass;
	size_t           i;
	int              res = 0;
	ir_graph        *rem = current_ir_graph;

	/* on all graphs: beware: number of irgs might be changed */
	for (i = 0; i < get_irp_n_irgs(); ++i) {
		ir_graph *irg = current_ir_graph = get_irp_irg(i);
		unsigned idx = mgr->run_idx;
		/* run every pass on every graph */
		list_for_each_entry(ir_graph_pass_t, pass, &mgr->passes, list) {
			int pass_res = pass->run_on_irg(irg, pass->context);
			if (pass_res != 0)
				res = 1;
			/* verify is necessary */
			if (mgr->verify_all) {
				if (pass->verify_irg) {
					pass->verify_irg(irg, pass->context);
				} else {
					irg_verify(irg, 0);
				}
			}
			/* dump */
			if (mgr->dump_all) {
				if (pass->dump_irg) {
					pass->dump_irg(irg, pass->context, idx);
				} else {
					char buf[1024];
					create_suffix(buf, sizeof(buf), pass->name);
					dump_ir_graph(irg, buf);
				}
			}
			++idx;
		}
	}
	current_ir_graph = rem;
	return res;
}

/**
 * Verify all graphs on the given ir_prog.
 */
static int irp_verify_irgs(void)
{
	int    res = 1;
	size_t i;
	size_t n_irgs = get_irp_n_irgs();

	for (i = 0; i < n_irgs; ++i)
		res &= irg_verify(get_irp_irg(i), 0);
	return res;
}

/* Run all passes of an ir_prog pass manager. */
int ir_prog_pass_mgr_run(ir_prog_pass_manager_t *mgr)
{
	ir_prog_pass_t *pass;
	int            res = 0;

	/* run every pass on every graph */
	unsigned idx = mgr->run_idx;
	list_for_each_entry(ir_prog_pass_t, pass, &mgr->passes, list) {
		int pass_res = pass->run_on_irprog(irp, pass->context);
		if (pass_res != 0)
			res = 1;
		/* verify is necessary */
		if (mgr->verify_all) {
			if (pass->verify_irprog) {
				pass->verify_irprog(irp, pass->context);
			} else {
				irp_verify_irgs();
			}
		}
		/* dump */
		if (mgr->dump_all) {
			if (pass->dump_irprog) {
				pass->dump_irprog(irp, pass->context, idx);
			} else {
				char buf[1024];
				create_suffix(buf, sizeof(buf), pass->name);
				dump_all_ir_graphs(buf);
			}
		}
		if (pass->is_wrapper) {
			ir_graph_pass_manager_t *graph_mgr = (ir_graph_pass_manager_t*)pass->context;
			idx += graph_mgr->n_passes;
		} else
			++idx;
	}
	return res;
}

/* Creates a new ir_graph pass manager. */
ir_graph_pass_manager_t *new_graph_pass_mgr(
	const char *name, int verify_all, int dump_all)
{
	ir_graph_pass_manager_t *res = XMALLOCZ(ir_graph_pass_manager_t);

	INIT_LIST_HEAD(&res->passes);
	res->kind       = k_ir_graph_pass_mgr;
	res->name       = name;
	res->run_idx    = 0;
	res->verify_all = verify_all != 0;
	res->dump_all   = dump_all   != 0;

	return res;
}

/* Creates a new ir_prog pass manager. */
ir_prog_pass_manager_t *new_prog_pass_mgr(
	const char *name, int verify_all, int dump_all)
{
	ir_prog_pass_manager_t *res = XMALLOCZ(ir_prog_pass_manager_t);

	INIT_LIST_HEAD(&res->passes);
	res->kind       = k_ir_prog_pass_mgr;
	res->name       = name;
	res->run_idx    = 0;
	res->verify_all = verify_all != 0;
	res->dump_all   = dump_all   != 0;

	return res;
}

/* Terminate an ir_graph pass manager and all owned passes. */
void term_graph_pass_mgr(ir_graph_pass_manager_t *mgr)
{
	ir_graph_pass_t *pass, *next;

	list_for_each_entry_safe(ir_graph_pass_t, pass, next, &mgr->passes, list) {
		if (pass->rem_from_mgr)
			pass->rem_from_mgr(pass->context);
		pass->kind = k_BAD;
		xfree(pass);
	}
	mgr->kind = k_BAD;
	xfree(mgr);
}

/* Terminate an ir_prog pass manager and all owned passes. */
void term_prog_pass_mgr(ir_prog_pass_manager_t *mgr)
{
	ir_prog_pass_t *pass, *next;

	list_for_each_entry_safe(ir_prog_pass_t, pass, next, &mgr->passes, list) {
		if (pass->rem_from_mgr)
			pass->rem_from_mgr(pass->context);
		pass->kind = k_BAD;
		xfree(pass);
	}
	mgr->kind = k_BAD;
	xfree(mgr);
}

/**
 * Set the run index for an irgraph pass manager.
 *
 * @param mgr      the manager
 * @param run_idx  the index for the first pass of this manager
 */
void ir_graph_pass_mgr_set_run_idx(
	ir_graph_pass_manager_t *mgr, unsigned run_idx)
{
	mgr->run_idx = run_idx;
}

/**
 * Set the run index for an irprog pass manager.
 *
 * @param mgr      the manager
 * @param run_idx  the index for the first pass of this manager
 */
void ir_prog_pass_mgr_set_run_idx(
	ir_prog_pass_manager_t *mgr, unsigned run_idx)
{
	mgr->run_idx = run_idx;
}

/**
 * Wrapper for running void function(ir_graph *irg) as an ir_graph pass.
 */
static int void_graph_wrapper(ir_graph *irg, void *context)
{
	void_pass_func_irg function = (void_pass_func_irg)context;
	function(irg);
	return 0;
}  /* void_graph_wrapper */

/* Creates an ir_graph pass for running void function(ir_graph *irg). */
ir_graph_pass_t *def_graph_pass(
	const char *name, void (*function)(ir_graph *irg))
{
	struct ir_graph_pass_t *pass = XMALLOCZ(ir_graph_pass_t);

	pass->kind       = k_ir_graph_pass;
	pass->run_on_irg = void_graph_wrapper;
	pass->context    = (void*)function;
	pass->name       = name;

	INIT_LIST_HEAD(&pass->list);

	return pass;
}  /* def_graph_pass */

/**
 * Wrapper for running int function(ir_graph *irg) as an ir_graph pass.
 */
static int int_graph_wrapper(ir_graph *irg, void *context)
{
	int_pass_func_irg function = (int_pass_func_irg)context;
	return function(irg);
}  /* int_graph_wrapper */

/* Creates an ir_graph pass for running void function(ir_graph *irg). */
ir_graph_pass_t *def_graph_pass_ret(
		const char *name, int (*function)(ir_graph *irg))
{
	struct ir_graph_pass_t *pass = XMALLOCZ(ir_graph_pass_t);

	pass->kind       = k_ir_graph_pass;
	pass->run_on_irg = int_graph_wrapper;
	pass->context    = (void*)function;
	pass->name       = name;

	INIT_LIST_HEAD(&pass->list);

	return pass;
}  /* def_graph_pass_ret */

/* constructor for a default graph pass */
ir_graph_pass_t *def_graph_pass_constructor(
	ir_graph_pass_t *pass,
	const char *name, int (*function)(ir_graph *irg, void *context)) {
	if (pass == NULL)
		pass = XMALLOCZ(ir_graph_pass_t);
	else
		memset(pass, 0, sizeof(ir_graph_pass_t));
	pass->kind       = k_ir_graph_pass;
	pass->run_on_irg = function;
	pass->context    = pass;
	pass->name       = name;

	INIT_LIST_HEAD(&pass->list);

	return pass;
}  /* def_graph_pass_constructor */

/* set the run parallel property */
void ir_graph_pass_set_parallel(ir_graph_pass_t *pass, int flag)
{
	pass->run_parallel = flag != 0;
}  /* ir_graph_pass_set_parallel */

/**
 * Wrapper for running void function(void) as an ir_prog pass.
 */
static int void_prog_wrapper(ir_prog *irp, void *context)
{
	void_pass_func function = (void_pass_func)context;

	(void)irp;
	function();
	return 0;
}  /* void_graph_wrapper */

/* Creates an ir_prog pass for running void function(void). */
ir_prog_pass_t *def_prog_pass(
	const char *name,
	void (*function)(void))
{
	struct ir_prog_pass_t *pass = XMALLOCZ(ir_prog_pass_t);

	pass->kind          = k_ir_prog_pass;
	pass->run_on_irprog = void_prog_wrapper;
	pass->context       = (void*)function;
	pass->name          = name;

	INIT_LIST_HEAD(&pass->list);

	return pass;
}  /* def_prog_pass */

/* Creates an ir_prog pass for running void function(void). */
ir_prog_pass_t *def_prog_pass_constructor(
	ir_prog_pass_t *pass,
	const char *name,
	int (*function)(ir_prog *irp, void *context))
{
	if (pass == NULL)
		pass = XMALLOCZ(ir_prog_pass_t);
	else
		memset(pass, 0, sizeof(ir_prog_pass_t));

	pass->kind          = k_ir_prog_pass;
	pass->run_on_irprog = function;
	pass->context       = pass;
	pass->name          = name;

	INIT_LIST_HEAD(&pass->list);

	return pass;
}  /* def_prog_pass_constructor */

typedef struct pass_t {
	ir_prog_pass_t pass;
	void           *context;
	void (*function)(void *context);
} pass_t;

/**
 * Wrapper for the call_function pass.
 */
static int call_function_wrapper(ir_prog *irp, void *context)
{
	pass_t *pass = (pass_t*)context;

	(void)irp;
	pass->function(pass->context);
	return 0;
}  /* call_function_wrapper */

ir_prog_pass_t *call_function_pass(
	const char *name, void (*function)(void *context), void *context) {
	struct pass_t *pass = XMALLOCZ(struct pass_t);

	def_prog_pass_constructor(
		&pass->pass, name ? name : "set_function", call_function_wrapper);

	pass->pass.verify_irprog = ir_prog_no_verify;
	pass->pass.dump_irprog   = ir_prog_no_dump;
	pass->pass.context       = pass;

	pass->function = function;
	pass->context  = context;

	return &pass->pass;
}  /* call_function_pass */
