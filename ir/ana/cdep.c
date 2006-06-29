#include <assert.h>
#include <stdlib.h>
#include "irdom.h"
#include "irgraph.h"
#include "irgwalk.h"
#include "irnode.h"
#include "pmap.h"
#include "xmalloc.h"
#include "cdep.h"
#include "irprintf.h"

typedef unsigned int uint;

static pmap *cdep_map;

cdep *find_cdep(const ir_node *block)
{
	return pmap_get(cdep_map, (void *)block);
}


void exchange_cdep(ir_node *old, const ir_node *nw)
{
	cdep *cdep = find_cdep(nw);

	pmap_insert(cdep_map, old, cdep);
}


static void add_cdep(ir_node* node, ir_node* dep_on)
{
	cdep *dep = find_cdep(node);
#if 0
	ir_fprintf(stderr, "Adding cdep of %+F on %+F\n", node, dep_on);
#endif

	if (dep == NULL) {
		cdep *newdep = xmalloc(sizeof(*newdep));

		newdep->node = dep_on;
		newdep->next = NULL;
		pmap_insert(cdep_map, node, newdep);
	} else {
		cdep *newdep;

		for (;;) {
			if (dep->node == dep_on) return;
			if (dep->next == NULL) break;
			dep = dep->next;
		}
		newdep = xmalloc(sizeof(*newdep));
		newdep->node = dep_on;
		newdep->next = NULL;
		dep->next = newdep;
	}
}

typedef struct cdep_env {
	ir_node *start_block;
	ir_node *end_block;
} cdep_env;

/**
 * Pre-block-walker: calculate the control dependence
 */
static void cdep_pre(ir_node *node, void *ctx)
{
	cdep_env *env = ctx;
	uint n;
	uint i;

	/* special case:
	 * start and end block have no control dependency
	 */
	if (node == env->start_block) return;
	if (node == env->end_block) return;

	n = get_Block_n_cfgpreds(node);
	for (i = 0; i < n; i++) {
		ir_node *pred = get_Block_cfgpred_block(node, i);
		ir_node *pdom;
		ir_node *dependee;

		if (is_Bad(pred)) continue;

		pdom = get_Block_ipostdom(pred);
		for (dependee = node; dependee != pdom; dependee = get_Block_ipostdom(dependee)) {
			assert(!is_Bad(pdom));
			add_cdep(dependee, pred);
		}
	}
}


#include "irdump.h"

/**
 * A block edge hook: add all cdep edges of block.
 */
static int cdep_edge_hook(FILE *F, ir_node *block)
{
	cdep *cd;

#if 0
	ir_node *pdom = get_Block_ipostdom(block);
	if (pdom != NULL) {
		fprintf(
			F,
			"edge:{sourcename:\"n%ld\" targetname:\"n%ld\" color:gold}\n",
			get_irn_node_nr(pdom), get_irn_node_nr(block)
		);
	}
#endif

	for (cd = find_cdep(block); cd != NULL; cd = cd->next) {
		fprintf(
			F,
			"edge:{sourcename:\"n%ld\" targetname:\"n%ld\" "
			"linestyle:dashed color:gold}\n",
			get_irn_node_nr(block), get_irn_node_nr(cd->node)
		);
	}

	return 0;
}


void compute_cdep(ir_graph *irg)
{
	ir_node *start_block, *rem;
	cdep_env env;

	cdep_map = pmap_create();

	assure_postdoms(irg);

	/* we must temporary change the post dominator relation */
	start_block = get_irg_start_block(irg);
	rem = get_Block_ipostdom(start_block);
	set_Block_ipostdom(start_block, get_irg_end_block(irg));

	env.start_block = get_irg_start_block(irg);
	env.end_block   = get_irg_end_block(irg);
	irg_block_walk_graph(irg, cdep_pre, NULL, &env);

#if 0
	set_dump_block_edge_hook(cdep_edge_hook);
	dump_ir_block_graph(irg, "_cdep");
	set_dump_block_edge_hook(NULL);
#endif

	/* restore the post dominator relation */
	set_Block_ipostdom(start_block, rem);
}


void free_cdep(ir_graph *irg)
{
	// TODO atm leaking more memory than a small memory leaking animal
}


int is_cdep_on(const ir_node *dependee, const ir_node *candidate)
{
	const cdep *dep;

	for (dep = find_cdep(dependee); dep != NULL; dep = dep->next) {
		if (dep->node == candidate) return 1;
	}
	return 0;
}


int is_iterated_cdep_on(ir_node *dependee, ir_node *candidate)
{
	const cdep *dep;

	while ((dep = find_cdep(dependee)) != NULL) {
		if (dep->next != NULL) return 0;
		if (dep->node == candidate) return 1;
		dependee = dep->node;
	}
	return 0;
}


ir_node *get_unique_cdep(const ir_node *block)
{
	cdep *cdep = find_cdep(block);

	return cdep != NULL && cdep->next == NULL ? cdep->node : NULL;
}


int has_multiple_cdep(const ir_node *block)
{
	cdep *cdep = find_cdep(block);

	return cdep != NULL && cdep->next != NULL;
}
