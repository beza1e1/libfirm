/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief     Helper function for integrated debug support
 * @author    Michael Beck
 * @date      2005
 */
#ifdef DEBUG_libfirm

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "debugger.h"

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <ctype.h>

#include "set.h"
#include "ident.h"
#include "irhooks.h"
#include "irgraph_t.h"
#include "entity_t.h"
#include "irprintf.h"
#include "irdump.h"
#include "iredges_t.h"
#include "debug.h"
#include "error.h"
#include "util.h"

#ifdef _WIN32
/* Break into the debugger. The Win32 way. */
void firm_debug_break(void)
{
	DebugBreak();
}
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64))
/* Break into the debugger. The ia32/x86_64 way under GCC. */
void firm_debug_break(void)
{
	__asm__ __volatile__("int3");
}
#else
/* Break into the debugger. Poor Unix way. */
void firm_debug_break(void)
{
	raise(SIGINT);
}
#endif /* _WIN32 */

/** supported breakpoint kinds */
typedef enum {
	BP_NR    = 'n',   /**< break on node number. */
	BP_IDENT = 'i'    /**< break on ident. */
} bp_kind;

/**
 * Reasons for node number breakpoints.
 */
typedef enum bp_reasons_t {
	BP_ON_NEW_THING,   /**< break if node, entity or type with number is created */
	BP_ON_REPLACE,     /**< break if node with number is replaced */
	BP_ON_LOWER,       /**< break if node with number is lowered */
	BP_ON_REMIRG,      /**< break if an IRG is removed */
	BP_ON_NEW_ENT,     /**< break if a new entity is created */
	BP_MAX_REASON
} bp_reasons_t;

/** A breakpoint. */
typedef struct breakpoint {
	bp_kind      kind;        /**< the kind of this break point */
	unsigned     bpnr;        /**< break point number */
	int          active;      /**< non-zero, if this break point is active */
	bp_reasons_t reason;      /**< reason for the breakpoint */
	struct breakpoint *next; /**< link to the next one */
} breakpoint;

/** A number breakpoint. */
typedef struct {
	breakpoint   bp;       /**< the breakpoint data */
	long         nr;       /**< the node number */
} bp_nr_t;

/** Calculate the hash value for a node breakpoint. */
#define HASH_NR_BP(key) (((key).nr << 2) ^ (key).bp.reason)

/** An ident breakpoint. */
typedef struct {
	breakpoint   bp;       /**< the breakpoint data */
	ident        *id;      /**< the ident */
} bp_ident_t;

/** Calculate the hash value for an ident breakpoint. */
#define HASH_IDENT_BP(key) (hash_ptr((key).id) ^ (key).bp.reason)

/** The set containing the breakpoints on node numbers. */
static set *bp_numbers;

/** The set containing the breakpoints on idents. */
static set *bp_idents;

/**< the list of all breakpoints */
static breakpoint *bp_list;

/** number of the current break point */
static unsigned bp_num = 0;

/** set if break on init command was issued. */
static int break_on_init = 0;

/** the hook entries for the Firm debugger module. */
static hook_entry_t debugger_hooks[hook_last];

/** number of active breakpoints to maintain hooks. */
static unsigned num_active_bp[BP_MAX_REASON];

/**
 * The debug message buffer
 */
static char firm_dbg_msg_buf[2048];

/**
 * If set, the debug extension writes all output to the
 * firm_dbg_msg_buf buffer
 */
static int redir_output = 0;

/**
 * Is set to one, if the debug extension is active
 */
static int is_active = 0;

/** hook the hook h with function fkt. */
#define HOOK(h, fkt) \
do {                                      \
	debugger_hooks[h].hook._##h = fkt;    \
	register_hook(h, &debugger_hooks[h]); \
} while (0)

/** unhook the hook h */
#define UNHOOK(h) \
do {                                        \
	unregister_hook(h, &debugger_hooks[h]); \
	debugger_hooks[h].hook._##h = NULL;     \
} while (0)

/** returns non-zero if a entry hook h is used */
#define IS_HOOKED(h) (debugger_hooks[h].hook._##h != NULL)

/* some macros needed to create the info string */
#define _DBG_VERSION(major, minor)  #major "." #minor
#define DBG_VERSION(major, minor)   _DBG_VERSION(major, minor)
#define API_VERSION(major, minor)   "API:" DBG_VERSION(major, minor)

/* the API version: change if needed */
#define FIRM_DBG_MAJOR  1
#define FIRM_DBG_MINOR  0

/** for automatic detection of the debug extension */
static const char __attribute__((used)) firm_debug_info_string[] =
	API_VERSION(FIRM_DBG_MAJOR, FIRM_DBG_MINOR);

int firm_debug_active(void)
{
	return is_active;
}

/**
 * Reset the debug text buffer.
 */
static void reset_dbg_buf(void)
{
	firm_dbg_msg_buf[0] = '\0';
}

const char *firm_debug_text(void)
{
	firm_dbg_msg_buf[sizeof(firm_dbg_msg_buf) - 1] = '\0';
	return firm_dbg_msg_buf;
}

/**
 * debug output
 */
static void dbg_printf(const char *fmt, ...)
{
	if (fmt[0] != '+')
		reset_dbg_buf();
	else
		++fmt;

	va_list args;
	va_start(args, fmt);
	if (redir_output) {
		size_t const cur = strlen(firm_dbg_msg_buf);
		ir_vsnprintf(firm_dbg_msg_buf + cur, sizeof(firm_dbg_msg_buf) - cur, fmt, args);
	} else {
		ir_vprintf(fmt, args);
	}
	va_end(args);
}

/**
 * A new node is created.
 *
 * @param ctx   the hook context
 * @param irg   the IR graph on which the node is created
 * @param node  the new IR node that was created
 */
static void dbg_new_node(void *ctx, ir_graph *irg, ir_node *node)
{
	bp_nr_t key, *elem;
	(void) ctx;
	(void) irg;

	key.nr        = get_irn_node_nr(node);
	key.bp.reason = BP_ON_NEW_THING;

	elem = set_find(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));
	if (elem && elem->bp.active) {
		dbg_printf("Firm BP %u reached, %+F created\n", elem->bp.bpnr, node);
		firm_debug_break();
	}
}

/**
 * A node is replaced.
 *
 * @param ctx   the hook context
 * @param old   the IR node the is replaced
 * @param nw    the new IR node that will replace old
 */
static void dbg_replace(void *ctx, ir_node *old, ir_node *nw)
{
	bp_nr_t key, *elem;
	(void) ctx;

	key.nr        = get_irn_node_nr(old);
	key.bp.reason = BP_ON_REPLACE;

	elem = set_find(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));
	if (elem && elem->bp.active) {
		dbg_printf("Firm BP %u reached, %+F will be replaced by %+F\n", elem->bp.bpnr, old, nw);
		firm_debug_break();
	}
}

/**
 * A new node is lowered.
 *
 * @param ctx   the hook context
 * @param node  the new IR node that will be lowered
 */
static void dbg_lower(void *ctx, ir_node *node)
{
	bp_nr_t key, *elem;
	(void) ctx;

	key.nr        = get_irn_node_nr(node);
	key.bp.reason = BP_ON_LOWER;

	elem = set_find(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));
	if (elem && elem->bp.active) {
		dbg_printf("Firm BP %u reached, %+F will be lowered\n", elem->bp.bpnr, node);
		firm_debug_break();
	}
}

/**
 * A graph will be deleted.
 *
 * @param ctx   the hook context
 * @param irg   the IR graph that will be deleted
 */
static void dbg_free_graph(void *ctx, ir_graph *irg)
{
	(void) ctx;
	{
		bp_nr_t key, *elem;
		key.nr        = get_irg_graph_nr(irg);
		key.bp.reason = BP_ON_REMIRG;

		elem = set_find(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));
		if (elem && elem->bp.active) {
			ir_printf("Firm BP %u reached, %+F will be deleted\n", elem->bp.bpnr, irg);
			firm_debug_break();
		}
	}
	{
		bp_ident_t key, *elem;
		ir_entity *ent = get_irg_entity(irg);

		if (! ent)
			return;

		key.id        = get_entity_ident(ent);
		key.bp.reason = BP_ON_REMIRG;

		elem = set_find(bp_ident_t, bp_idents, &key, sizeof(key), HASH_IDENT_BP(key));
		if (elem && elem->bp.active) {
			dbg_printf("Firm BP %u reached, %+F will be deleted\n", elem->bp.bpnr, ent);
			firm_debug_break();
		}
	}
}

/**
 * An entity was created.
 *
 * @param ctx   the hook context
 * @param ent   the newly created entity
 */
static void dbg_new_entity(void *ctx, ir_entity *ent)
{
	(void) ctx;
	{
		bp_ident_t key, *elem;

		key.id        = get_entity_ident(ent);
		key.bp.reason = BP_ON_NEW_ENT;

		elem = set_find(bp_ident_t, bp_idents, &key, sizeof(key), HASH_IDENT_BP(key));
		if (elem && elem->bp.active) {
			ir_printf("Firm BP %u reached, %+F was created\n", elem->bp.bpnr, ent);
			firm_debug_break();
		}
	}
	{
		bp_nr_t key, *elem;

		key.nr        = get_entity_nr(ent);
		key.bp.reason = BP_ON_NEW_THING;

		elem = set_find(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));
		if (elem && elem->bp.active) {
			dbg_printf("Firm BP %u reached, %+F was created\n", elem->bp.bpnr, ent);
			firm_debug_break();
		}
	}
}

/**
 * A type was created.
 *
 * @param ctx   the hook context
 * @param tp    the newly created type
 */
static void dbg_new_type(void *ctx, ir_type *tp)
{
	(void) ctx;
	{
		bp_nr_t key, *elem;

		key.nr        = get_type_nr(tp);
		key.bp.reason = BP_ON_NEW_THING;

		elem = set_find(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));
		if (elem && elem->bp.active) {
			ir_printf("Firm BP %u reached, %+F was created\n", elem->bp.bpnr, tp);
			firm_debug_break();
		}
	}
}

/**
 * Return the reason string.
 */
static const char *reason_str(bp_reasons_t reason)
{
	switch (reason) {
	case BP_ON_NEW_THING: return "node, entity or type creation";
	case BP_ON_REPLACE:   return "node replace";
	case BP_ON_LOWER:     return "node lowering";
	case BP_ON_REMIRG:    return "removing IRG";
	case BP_ON_NEW_ENT:   return "entity creation";
	case BP_MAX_REASON:   break;
	}
	panic("unsupported reason");
}

/**
 * Compare two number breakpoints.
 */
static int cmp_nr_bp(const void *elt, const void *key, size_t size)
{
	const bp_nr_t *e1 = (const bp_nr_t*)elt;
	const bp_nr_t *e2 = (const bp_nr_t*)key;
	(void) size;

	return (e1->nr - e2->nr) | (e1->bp.reason - e2->bp.reason);
}

/**
 * Compare two ident breakpoints.
 */
static int cmp_ident_bp(const void *elt, const void *key, size_t size)
{
	const bp_ident_t *e1 = (const bp_ident_t*)elt;
	const bp_ident_t *e2 = (const bp_ident_t*)key;
	(void) size;

	return (e1->id != e2->id) | (e1->bp.reason - e2->bp.reason);
}

/**
 * Update the hooks.
 */
static void update_hooks(breakpoint *bp)
{
#define CASE_ON(a, hook, handler)  case a: if (! IS_HOOKED(hook)) HOOK(hook, handler); break
#define CASE_OFF(a, hook) case a: if (IS_HOOKED(hook)) UNHOOK(hook); break

	if (bp->active)
		++num_active_bp[bp->reason];
	else
		--num_active_bp[bp->reason];

	if (num_active_bp[bp->reason] > 0) {
		/* register the hooks on demand */
		switch (bp->reason) {
		CASE_ON(BP_ON_REPLACE, hook_replace,    dbg_replace);
		CASE_ON(BP_ON_LOWER,   hook_lower,      dbg_lower);
		CASE_ON(BP_ON_REMIRG,  hook_free_graph, dbg_free_graph);
		CASE_ON(BP_ON_NEW_ENT, hook_new_entity, dbg_new_entity);
		case BP_ON_NEW_THING:
			if (!IS_HOOKED(hook_new_node))
				HOOK(hook_new_node, dbg_new_node);
			if (!IS_HOOKED(hook_new_type))
				HOOK(hook_new_type, dbg_new_type);
			if (!IS_HOOKED(hook_new_entity))
				HOOK(hook_new_entity, dbg_new_entity);
			break;
		default:
			break;
		}
	}
	else {
		/* unregister the hook on demand */
		switch (bp->reason) {
		CASE_OFF(BP_ON_REPLACE,  hook_replace);
		CASE_OFF(BP_ON_LOWER,    hook_lower);
		CASE_OFF(BP_ON_REMIRG,   hook_free_graph);
		CASE_OFF(BP_ON_NEW_ENT,  hook_new_entity);
		case BP_ON_NEW_THING:
			if (IS_HOOKED(hook_new_node))
				UNHOOK(hook_new_node);
			if (IS_HOOKED(hook_new_type))
				UNHOOK(hook_new_type);
			if (IS_HOOKED(hook_new_entity))
				UNHOOK(hook_new_entity);
			break;
		default:
			break;
		}
	}
#undef CASE_ON
#undef CASE_OFF
}

/**
 * Break if nr is reached.
 */
static void break_on_nr(long nr, bp_reasons_t reason)
{
	bp_nr_t key, *elem;

	key.bp.kind   = BP_NR;
	key.bp.bpnr   = 0;
	key.bp.active = 1;
	key.bp.reason = reason;
	key.nr        = nr;

	elem = set_insert(bp_nr_t, bp_numbers, &key, sizeof(key), HASH_NR_BP(key));

	if (elem->bp.bpnr == 0) {
		/* new break point */
		elem->bp.bpnr = ++bp_num;
		elem->bp.next = bp_list;
		bp_list = &elem->bp;

		dbg_printf("Firm BP %u: %s of Nr %ld\n", elem->bp.bpnr, reason_str(reason), nr);

		update_hooks(&elem->bp);
	}
}

/**
 * Break if ident name is reached.
 */
static void break_on_ident(const char *name, bp_reasons_t reason)
{
	bp_ident_t key, *elem;

	key.bp.kind   = BP_IDENT;
	key.bp.bpnr   = 0;
	key.bp.active = 1;
	key.bp.reason = reason;
	key.id        = new_id_from_str(name);

	elem = set_insert(bp_ident_t, bp_idents, &key, sizeof(key), HASH_IDENT_BP(key));

	if (elem->bp.bpnr == 0) {
		/* new break point */
		elem->bp.bpnr = ++bp_num;
		elem->bp.next = bp_list;
		bp_list = &elem->bp;

		dbg_printf("Firm BP %u: %s of ident \"%s\"\n", elem->bp.bpnr, reason_str(reason), name);

		update_hooks(&elem->bp);
	}
}

/**
 * Sets/resets the active flag of breakpoint bp.
 */
static void bp_activate(unsigned bp, int active)
{
	breakpoint *p;

	for (p = bp_list; p; p = p->next) {
		if (p->bpnr == bp) {
			if (p->active != active) {
				p->active = active;
				update_hooks(p);
			}

			dbg_printf("Firm BP %u is now %s\n", bp, active ? "enabled" : "disabled");
			return;
		}
	}
	dbg_printf("Error: Firm BP %u not exists.\n", bp);
}


/**
 * Show a list of supported commands
 */
static void show_commands(void)
{
	dbg_printf("Internal Firm debugger extension commands:\n"
		"init                  break after initialization\n"
		"create nr             break if node nr was created\n"
		"replace nr            break if node nr is replaced by another node\n"
		"lower nr              break before node nr is lowered\n"
		"remirg nr|name        break if the irg of nr or entity name is deleted\n"
		"newent nr|name        break if the entity nr or name was created\n"
		"newtype nr|name       break if the type nr or name was created\n"
		"bp                    show all breakpoints\n"
		"enable nr             enable breakpoint nr\n"
		"disable nr            disable breakpoint nr\n"
		"showtype nr|name      show content of the type nr or name\n"
		"showent nr|name       show content of the entity nr or name\n"
		"setmask name msk      sets the debug module name to mask msk\n"
		"setlvl  name lvl      sets the debug module name to level lvl\n"
		"setoutfile name file  redirects debug output of module name to file\n"
		"irgname name          prints address and graph number of a method given by its name\n"
		"irgldname ldname      prints address and graph number of a method given by its ldname\n"
		"initialnodenr n|rand  set initial node number to n or random number\n"
		"help                  list all commands\n"
		);
}

/**
 * Shows all Firm breakpoints.
 */
static void show_bp(void)
{
	breakpoint *p;
	bp_nr_t  *node_p;
	bp_ident_t *ident_p;
	int have_one = 0;

	dbg_printf("Firm Breakpoints:");
	for (p = bp_list; p; p = p->next) {
		have_one = 1;
		dbg_printf("+\n  BP %u: ", p->bpnr);

		switch (p->kind) {
		case BP_NR:
			node_p = (bp_nr_t *)p;
			dbg_printf("%s of Nr %ld ", reason_str(p->reason), node_p->nr);
			break;

		case BP_IDENT:
			ident_p = (bp_ident_t *)p;
			dbg_printf("+%s of ident \"%s\" ", reason_str(p->reason), get_id_str(ident_p->id));
			break;
		}

		dbg_printf(p->active ? "+enabled" : "+disabled");
	}
	dbg_printf(have_one ? "+\n" : "+ NONE\n");
}

/**
 * firm_dbg_register() expects that the name is stored persistent.
 * So we need this little helper function
 */
static firm_dbg_module_t *dbg_register(const char *name)
{
	ident *id = new_id_from_str(name);

	return firm_dbg_register(get_id_str(id));
}

/**
 * Sets the debug mask of module name to lvl
 */
static void set_dbg_level(const char *name, unsigned lvl)
{
	firm_dbg_module_t *module = dbg_register(name);

	if (firm_dbg_get_mask(module) != lvl) {
		firm_dbg_set_mask(module, lvl);

		dbg_printf("Setting debug mask of module %s to %u\n", name, lvl);
	}
}

/**
 * Redirects the debug output of module name to fname
 */
static void set_dbg_outfile(const char *name, const char *fname)
{
	firm_dbg_module_t *module = dbg_register(name);
	FILE *f = fopen(fname, "w");

	if (! f) {
		perror(fname);
		return;
	}

	firm_dbg_set_file(module, f);
	dbg_printf("Redirecting debug output of module %s to file %s\n", name, fname);
}

/**
 * Show info about a firm thing.
 */
static void show_firm_object(void *firm_thing)
{
	FILE *f = stdout;

	if (firm_thing == NULL) {
		fprintf(f, "<NULL>\n");
		return;
	}
	switch (get_kind(firm_thing)) {
	case k_BAD:
		fprintf(f, "BAD: (%p)\n", firm_thing);
		break;
	case k_entity:
		dump_entity_to_file(f, (ir_entity*)firm_thing);
		break;
	case k_type:
		dump_type_to_file(f, (ir_type*)firm_thing);
		break;
	case k_ir_graph:
	case k_ir_node:
	case k_ir_mode:
	case k_ir_op:
	case k_tarval:
	case k_ir_loop:
	case k_ir_prog:
		fprintf(f, "NIY\n");
		break;
	default:
		fprintf(f, "Cannot identify thing at (%p).\n", firm_thing);
	}
}

/**
 * Find a firm type by its number.
 */
static ir_type *find_type_nr(long nr)
{
	int i, n = get_irp_n_types();
	ir_type *tp;

	for (i = 0; i < n; ++i) {
		tp = get_irp_type(i);
		if (get_type_nr(tp) == nr)
			return tp;
	}
	tp = get_glob_type();
	if (get_type_nr(tp) == nr)
		return tp;
	return NULL;
}

/**
 * Find a firm type by its name.
 */
static ir_type *find_type_name(const char *name)
{
	int i, n = get_irp_n_types();
	ir_type *tp;

	for (i = 0; i < n; ++i) {
		tp = get_irp_type(i);
		if (!is_compound_type(tp))
			continue;

		if (strcmp(get_compound_name(tp), name) == 0)
			return tp;
	}
	tp = get_glob_type();
	if (strcmp(get_compound_name(tp), name) == 0)
		return tp;
	return NULL;
}

/** The environment for the entity search functions. */
typedef struct find_env {
	union {
		long        nr;   /**< the number that is searched for */
		const char *name; /**< the name that is searched for */
	} u;
	ir_entity *res;     /**< the result */
} find_env_t;

/**
 * Type-walker: Find an entity with given number.
 */
static void check_ent_nr(type_or_ent tore, void *ctx)
{
	find_env_t *env = (find_env_t*)ctx;

	if (is_entity(tore.ent)) {
		if (get_entity_nr(tore.ent) == env->u.nr) {
			env->res = tore.ent;
		}
	}
}

/**
 * Type-walker: Find an entity with given name.
 */
static void check_ent_name(type_or_ent tore, void *ctx)
{
	find_env_t *env = (find_env_t*)ctx;

	if (is_entity(tore.ent))
		if (strcmp(get_entity_name(tore.ent), env->u.name) == 0) {
			env->res = tore.ent;
		}
}

/**
 * Find a firm entity by its number.
 */
static ir_entity *find_entity_nr(long nr)
{
	find_env_t env;

	env.u.nr = nr;
	env.res  = NULL;
	type_walk(check_ent_nr, NULL, &env);
	return env.res;
}

/**
 * Find a firm entity by its name.
 */
static ir_entity *find_entity_name(const char *name)
{
	find_env_t env;

	env.u.name = name;
	env.res    = NULL;
	type_walk(check_ent_name, NULL, &env);
	return env.res;
}

/**
 * Search methods for a name.
 */
static void show_by_name(type_or_ent tore, void *env)
{
	ident *id = (ident *)env;

	if (is_entity(tore.ent)) {
		ir_entity *ent = tore.ent;

		if (is_method_entity(ent)) {
			if (get_entity_ident(ent) == id) {
				ir_type *owner = get_entity_owner(ent);
				ir_graph *irg = get_entity_irg(ent);

				if (owner != get_glob_type()) {
					printf("%s::%s", get_compound_name(owner), get_id_str(id));
				} else {
					printf("%s", get_id_str(id));
				}
				if (irg)
					printf("[%ld] (%p)\n", get_irg_graph_nr(irg), (void*)irg);
				else
					printf(" NULL\n");
			}
		}
	}
}

/**
 * Search methods for a ldname.
 */
static void show_by_ldname(type_or_ent tore, void *env)
{
	ident *id = (ident *)env;

	if (is_entity(tore.ent)) {
		ir_entity *ent = tore.ent;

		if (is_method_entity(ent)) {
			if (get_entity_ld_ident(ent) == id) {
				ir_type *owner = get_entity_owner(ent);
				ir_graph *irg = get_entity_irg(ent);

				if (owner != get_glob_type()) {
					printf("%s::%s", get_compound_name(owner), get_id_str(id));
				} else {
					printf("%s", get_id_str(id));
				}
				if (irg)
					printf("[%ld] (%p)\n", get_irg_graph_nr(irg), (void*)irg);
				else
					printf(" NULL\n");
			}
		}
	}
}

/**
 * prints the address and graph number of all irgs with given name
 */
static void irg_name(const char *name)
{
	ident *id = new_id_from_str(name);

	type_walk(show_by_name, NULL, (void *)id);
}

/**
 * prints the address and graph number of all irgs with given ld_name
 */
static void irg_ld_name(const char *name)
{
	ident *id = new_id_from_str(name);

	type_walk(show_by_ldname, NULL, (void *)id);
}

enum tokens {
	first_token = 256,
	tok_bp = first_token,
	tok_create,
	tok_disable,
	tok_dumpfilter,
	tok_enable,
	tok_help,
	tok_init,
	tok_irgldname,
	tok_irgname,
	tok_lower,
	tok_newent,
	tok_remirg,
	tok_replace,
	tok_setlvl,
	tok_setmask,
	tok_setoutfile,
	tok_showent,
	tok_showtype,
	tok_initialnodenr,
	tok_identifier,
	tok_number,
	tok_eof,
	tok_error
};

static const char *reserved[] = {
	"bp",
	"create",
	"disable",
	"dumpfilter",
	"enable",
	"help",
	"init",
	"irgldname",
	"irgname",
	"lower",
	"newent",
	"remirg",
	"replace",
	"setlvl",
	"setmask",
	"setoutfile",
	"showent",
	"showtype",
	"initialnodenr",
};

/**
 * The Lexer data.
 */
static struct lexer {
	int has_token;        /**< set if a token is cached. */
	unsigned cur_token;   /**< current token. */
	unsigned number;      /**< current token attribute. */
	const char *s;        /**< current token attribute. */
	size_t len;           /**< current token attribute. */

	const char *curr_pos;
	const char *end_pos;
	const char *tok_start;
} lexer;

/**
 * Initialize the lexer.
 */
static void init_lexer(const char *input)
{
	lexer.has_token = 0;
	lexer.curr_pos  = input;
	lexer.end_pos   = input + strlen(input);
}


/**
 * Get the next char from the input.
 */
static char next_char(void)
{
	if (lexer.curr_pos >= lexer.end_pos)
		return '\0';
	return *lexer.curr_pos++;
}

#define unput()    if (lexer.curr_pos < lexer.end_pos) --lexer.curr_pos

/**
 * The lexer.
 */
static unsigned get_token(void)
{
	char c;
	size_t i;

	/* skip white space */
	do {
		c = next_char();
	} while (c != '\0' && isspace((unsigned char)c));

	lexer.tok_start = lexer.curr_pos - 1;
	if (c == '.' || isalpha((unsigned char)c)) {
		/* command begins here */
		int         len = 0;
		const char* tok_start;

		do {
			c = next_char();
			++len;
		} while (isgraph((unsigned char)c));
		unput();

		tok_start = lexer.tok_start;
		if (*tok_start == '.') {
			++tok_start;
			--len;
		}
		for (i = ARRAY_SIZE(reserved); i-- != 0;) {
			if (strncasecmp(tok_start, reserved[i], len) == 0 && reserved[i][len] == '\0')
				return first_token + i;
		}

		/* identifier */
		lexer.s   = lexer.tok_start;
		lexer.len = lexer.curr_pos - lexer.s;
		return tok_identifier;
	} else if (isdigit((unsigned char)c) || c == '-') {
		unsigned number = 0;
		unsigned sign   = 0;

		/* we support negative numbers as well, so one can easily set all bits with -1 */
		if (c == '-') {
			sign = 1;
			c    = next_char();
		}

		if (c == '0') {
			c = next_char();

			if (c == 'x' || c == 'X') {
				for (;;) {
					c = next_char();

					if (! isxdigit((unsigned char)c))
						break;
					if (isdigit((unsigned char)c))
						number = (number << 4) | (c - '0');
					else
						number = (number << 4) | (toupper((unsigned char)c) - 'A' + 10);
				}
				unput();
				lexer.number = number;
				return tok_number;
			}
		}
		for (;;) {
			if (! isdigit((unsigned char)c))
				break;
			number = number * 10 + (c - '0');
			c = next_char();
		}
		unput();
		lexer.number = sign ? 0 - number : number;
		return tok_number;
	}
	else if (c == '\0')
		return tok_eof;
	return c;
}

void firm_debug(const char *cmd)
{
	char name[1024], fname[1024];
	size_t len;

	init_lexer(cmd);

	for (;;) {
		unsigned token;

		token = get_token();

		switch (token) {
		case tok_eof:
			goto leave;

		case tok_create:
			token = get_token();
			if (token != tok_number)
				goto error;
			break_on_nr(lexer.number, BP_ON_NEW_THING);
			break;

		case tok_replace:
			token = get_token();
			if (token != tok_number)
				goto error;
			break_on_nr(lexer.number, BP_ON_REPLACE);
			break;

		case tok_lower:
			token = get_token();
			if (token != tok_number)
				goto error;
			break_on_nr(lexer.number, BP_ON_LOWER);
			break;

		case tok_remirg:
			token = get_token();

			if (token == tok_number)
				break_on_nr(lexer.number, BP_ON_REMIRG);
			else if (token == tok_identifier) {
				len = MIN(lexer.len, 1023);
				strncpy(name, lexer.s, len);
				name[len] = '\0';
				break_on_ident(name, BP_ON_REMIRG);
			} else
				goto error;
			break;

		case tok_newent:
			token = get_token();

			if (token == tok_number)
				break_on_nr(lexer.number, BP_ON_NEW_THING);
			else if (token == tok_identifier) {
				len = MIN(lexer.len, 1023);
				strncpy(name, lexer.s, len);
				name[len] = '\0';
				break_on_ident(name, BP_ON_NEW_ENT);
			} else
				goto error;
			break;

		case tok_showtype:
			token = get_token();

			if (token == tok_number)
				show_firm_object(find_type_nr(lexer.number));
			else if (token == tok_identifier) {
				len = MIN(lexer.len, 1023);
				strncpy(name, lexer.s, len);
				name[len] = '\0';
				show_firm_object(find_type_name(name));
			} else
				goto error;
			break;

		case tok_showent:
			token = get_token();

			if (token == tok_number)
				show_firm_object(find_entity_nr(lexer.number));
			else if (token == tok_identifier) {
				len = MIN(lexer.len, 1023);
				strncpy(name, lexer.s, len);
				name[len] = '\0';
				show_firm_object(find_entity_name(name));
			} else
				goto error;
			break;

		case tok_init:
			break_on_init = 1;
			break;

		case tok_bp:
			show_bp();
			break;

		case tok_enable:
			token = get_token();
			if (token != tok_number)
				goto error;
			bp_activate(lexer.number, 1);
			break;

		case tok_disable:
			token = get_token();
			if (token != tok_number)
				goto error;
			bp_activate(lexer.number, 0);
			break;

		case tok_setmask:
			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(name, lexer.s, len);
			name[len] = '\0';

			token = get_token();
			if (token != tok_number)
				goto error;
			set_dbg_level(name, lexer.number);
			break;

		case tok_setlvl:
			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(name, lexer.s, len);
			name[len] = '\0';

			token = get_token();
			if (token != tok_number)
				goto error;
			set_dbg_level(name, (1 << lexer.number) - 1);
			break;

		case tok_setoutfile:
			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(name, lexer.s, len);
			name[len] = '\0';

			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(fname, lexer.s, len);
			fname[len] = '\0';
			set_dbg_outfile(name, fname);
			break;

		case tok_irgname:
			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(name, lexer.s, len);
			name[len] = '\0';
			irg_name(name);
			break;

		case tok_initialnodenr:
			token = get_token();
			if (token == tok_number) {
				dbg_printf("Setting initial node number to %u\n", lexer.number);
				irp->max_node_nr = lexer.number;
			} else if (token == tok_identifier && !strcmp(lexer.s, "rand")) {
				dbg_printf("Randomizing initial node number\n");
				srand(time(0));
				irp->max_node_nr += rand() % 6666;
			} else
				goto error;
			break;

		case tok_irgldname:
			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(name, lexer.s, len);
			name[len] = '\0';
			irg_ld_name(name);
			break;

		case tok_dumpfilter:
			token = get_token();
			if (token != tok_identifier)
				goto error;
			len = MIN(lexer.len, 1023);
			strncpy(name, lexer.s, len);
			name[len] = '\0';
			ir_set_dump_filter(name);
			break;

		case tok_help:
			show_commands();
			break;

		case tok_error:
		default:
error:
			printf("Error: before %s\n", lexer.tok_start);
			show_commands();
			goto leave;
		}

		token = get_token();
		if (token == tok_eof)
			break;
		if (token != ';')
			goto error;
	}
leave:
	;
}

void firm_init_debugger(void)
{
	char *env;

	bp_numbers = new_set(cmp_nr_bp, 8);
	bp_idents  = new_set(cmp_ident_bp, 8);

	env = getenv("FIRMDBG");

	is_active = 1;

	if (env)
		firm_debug(env);

	if (break_on_init)
		firm_debug_break();
}

void firm_finish_debugger(void)
{
	del_set(bp_numbers);
	del_set(bp_idents);
}

/**
 * A gdb helper function to print firm objects.
 */
const char *gdb_node_helper(void *firm_object)
{
	static char buf[1024];
	ir_snprintf(buf, sizeof(buf), "%+F", firm_object);
	return buf;
}

const char *gdb_tarval_helper(void *tv_object)
{
	static char buf[1024];
	ir_snprintf(buf, sizeof(buf), "%+T", tv_object);
	return buf;
}

const char *gdb_out_edge_helper(const ir_node *node)
{
	static char buf[4*1024];
	char *b = buf;
	size_t l;
	size_t len = sizeof(buf);
	foreach_out_edge(node, edge) {
		ir_node *n = get_edge_src_irn(edge);

		ir_snprintf(b, len, "%+F  ", n);
		l = strlen(b);
		len -= l;
		b += l;
	}

	return buf;
}

#else

/* some picky compiler do not allow empty files */
static int __attribute__((unused)) _firm_only_that_you_can_compile_with_NDEBUG_defined;

#endif /* NDEBUG */

/**
 * @page debugger   The Firm debugger extension
 *
 * Firm contains a debugger extension. This allows to set debugger breakpoints
 * an various events.
 * The extension uses a text interface which can be accessed from most debuggers.
 * More than one command can be given separated by ';'.
 *
 * @section sec_cmd Supported commands
 *
 * Historically all debugger commands start with a dot.  This isn't needed in newer
 * versions, but still supported, ie the commands ".init" and "init" are equal.
 * The following commands are currently supported:
 *
 * @b init
 *
 * Break immediately after the debugger extension was initialized.
 * Typically this command is used in the environment to stop the execution
 * of a Firm compiler right after the initialization, like this:
 *
 * $export FIRMDBG=".init"
 *
 * @b create nr
 *
 * Break if a new IR-node with node number nr was created.
 * Typically used to find the place where wrong nodes are created.
 *
 * @b replace nr
 *
 * Break before IR-node with node number nr is replaced by another node.
 *
 * @b lower nr
 *
 * Break before IR-node with node number nr is lowered.
 *
 * @b remirg nr
 *
 * Break if the irg with graph number nr is deleted.
 *
 * @b remirg name
 *
 * Break if the irg of entity name is deleted.
 *
 * @b newent nr
 *
 * Break if the entity with number nr was created.
 *
 * @b newent name
 *
 * Break if the entity name was created.
 *
 * @b newtype nr
 *
 * Break if the type with number nr was created.
 *
 * @b newtype name
 *
 * Break if the type name was created.
 *
 * @b bp
 *
 * Show all Firm internal breakpoints.
 *
 * @b enable nr
 *
 * Enables breakpoint nr.
 *
 * @b disable nr
 *
 * Disables breakpoint nr.
 *
 * @b showent nr
 *
 * Show the content of entity nr.
 *
 * @b showent name
 *
 * Show the content of entity name.
 *
 * @b showtype nr
 *
 * Show the content of type nr.
 *
 * @b showtype name
 *
 * Show the content of type name.
 *
 * @b setmask name msk
 *
 * Sets the debug module name to mask msk.
 *
 * @b setlvl name lvl
 *
 * Sets the debug module name to level lvl.
 *
 * @b setoutfile name file
 *
 * Redirects debug output of module name to file.
 *
 * @b irgname name
 *
 * Prints address and graph number of a method given by its name.
 *
 * @b irgldname name
 *
 * Prints address and graph number of a method given by its linker name.
 *
 * @b help
 *
 * List all commands.
 *
 *
 * The Firm debugger extension can be accessed using the function firm_debug().
 * The following example shows how to set a creation breakpoint in GDB when
 * node 2101 is created.
 *
 * -# set FIRMDBG="init"
 * -# start gdb with your compiler
 * -# after gdb breaks, issue
 *
 * call firm_debug("create 2101")
 *
 * On the console the following text should be issued:
 *
 * Firm BP 1: creation of Node 2101
 *
 *
 * @section gdb_macro GDB macro
 *
 * Add the following to your .gdbinit file:
 * @code
 #
 # define firm "cmd"  Firm debugger extension
 #
 define firm
 call firm_debug($arg0)
 end
 * @endcode
 *
 * Then, all Firm debugger extension commands can be accessed in the gdb
 * console using the firm prefix, eg.:
 *
 * firm "create 2101"
 *
 * firm "help"
 */
