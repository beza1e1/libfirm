/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Paint chordal graphs.
 * @author      Sebastian Hack
 * @date        12.05.2005
 */
#include <limits.h>

#include "pmap.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "iredges_t.h"
#include "util.h"

#include "bearch.h"
#include "belive_t.h"
#include "bechordal_t.h"
#include "besched.h"
#include "bechordal_draw.h"
#include "beirg.h"

typedef struct {
	be_chordal_env_t *env;
	plotter_t inh;
	const color_t *color;
	int width;
} base_plotter_t;

#define decl_self(type, from) \
  type *self = (type *) from

static void set_color(plotter_t *_self, const color_t *color)
{
	decl_self(base_plotter_t, _self);
	self->color = color;
}

static const color_t *get_color(const plotter_t *_self)
{
	decl_self(const base_plotter_t, _self);
	return self->color;
}

static void set_width(plotter_t *_self, int width)
{
	decl_self(base_plotter_t, _self);
	self->width = width;
}

static int get_width(const plotter_t *_self)
{
	decl_self(const base_plotter_t, _self);
	return self->width;
}

static void plotter_default_free(plotter_t *self)
{
	(void) self;
}

typedef struct {
	base_plotter_t inh;
	const char     *filename;
	FILE           *f;
} ps_plotter_t;


/*
  ____  ____    ____  _       _   _
 |  _ \/ ___|  |  _ \| | ___ | |_| |_ ___ _ __
 | |_) \___ \  | |_) | |/ _ \| __| __/ _ \ '__|
 |  __/ ___) | |  __/| | (_) | |_| ||  __/ |
 |_|   |____/  |_|   |_|\___/ \__|\__\___|_|

*/

static void ps_begin(plotter_t *_self, const rect_t *vis)
{
	FILE *f;
	decl_self(ps_plotter_t, _self);

	f = self->f = fopen(self->filename, "wt");
	fprintf(f, "%%!PS-Adobe-2.0\n");
	fprintf(f, "%%%%BoundingBox: %d %d %d %d\n", vis->x, vis->y, vis->w, vis->h);
}

static void ps_setcolor(plotter_t *_self, const color_t *color)
{
	decl_self(ps_plotter_t, _self);
	set_color(_self, color);

	fprintf(self->f, "%.2f %.2f %.2f setrgbcolor\n",
		color->r, color->g, color->b);
}

static void ps_line(plotter_t *_self, int x1, int y1, int x2, int y2)
{
	decl_self(ps_plotter_t, _self);

	fprintf(self->f, "%d %d moveto\n", x1, y1);
	fprintf(self->f, "%d %d lineto\n", x2, y2);
	fprintf(self->f, "stroke\n");
}

static void ps_box(plotter_t *_self, const rect_t *rect)
{
	decl_self(ps_plotter_t, _self);

	fprintf(self->f, "%d %d %d %d rectstroke\n",
		rect->x, rect->y, rect->w, rect->h);
}

static void ps_text(plotter_t *_self, int x, int y, const char *str)
{
	decl_self(ps_plotter_t, _self);

	fprintf(self->f, "%d %d moveto\n", x, y);
	fprintf(self->f, "(%s) show\n", str);
}

static void ps_finish(plotter_t *_self)
{
	decl_self(ps_plotter_t, _self);
	fclose(self->f);
}

static const plotter_if_t ps_plotter_vtab = {
	ps_begin,
	ps_setcolor,
	get_color,
	set_width,
	get_width,
	ps_line,
	ps_box,
	ps_text,
	ps_finish,
	plotter_default_free
};

plotter_t *new_plotter_ps(const char *filename)
{
	ps_plotter_t *ps_plotter = XMALLOC(ps_plotter_t);
	plotter_t *p = (plotter_t *) ps_plotter;

	ps_plotter->filename = filename;
	p->vtab = &ps_plotter_vtab;
	return p;
}

extern void plotter_free(plotter_t *self)
{
	self->vtab->free(self);
	free(self);
}

const draw_chordal_opts_t draw_chordal_def_opts = {
	10, 10, 30, 8, 10, 10
};

typedef struct draw_chordal_env_t {
	const be_chordal_env_t      *chordal_env;
	const arch_register_class_t *cls;
	pmap                        *block_dims;
	plotter_t                   *plotter;
	const draw_chordal_opts_t   *opts;
	struct obstack              obst;
} draw_chordal_env_t;

struct block_dims {
	rect_t   box;
	rect_t   subtree_box;
};

#define doz(a, b) MAX((a) - (b), 0)

static void block_dims_walker(ir_node *block, void *data)
{
	draw_chordal_env_t        *env  = (draw_chordal_env_t*)data;
	struct list_head          *head = get_block_border_head(env->chordal_env, block);
	const draw_chordal_opts_t *opts = env->opts;
	struct block_dims         *dims = OALLOCZ(&env->obst, struct block_dims);

	unsigned max_color = 0;
	unsigned max_step  = 0;
	foreach_border_head(head, b) {
		unsigned const col = arch_get_irn_register(b->irn)->index;
		max_color = MAX(max_color, col);
		max_step  = MAX(max_step,  b->step);
	}

	dims->box.w = (max_color + 2) * opts->h_inter_gap;
	dims->box.h =  max_step       * opts->v_inter_gap;

	pmap_insert(env->block_dims, block, dims);
}

static void layout(const draw_chordal_env_t *env, ir_node *bl, int x)
{
	const draw_chordal_opts_t *opts   = env->opts;
	struct block_dims         *dims   = pmap_get(struct block_dims, env->block_dims, bl);
	rect_t                    *rect   = &dims->subtree_box;
	int                       h_space = 0;
	int                       v_space = 0;

	memset(rect, 0, sizeof(*rect));
	rect->x = x;

	dominates_for_each(bl, sub) {
		struct block_dims *bl_dim = pmap_get(struct block_dims, env->block_dims, sub);

		layout(env, sub, rect->x + rect->w);

		rect->w += h_space + bl_dim->subtree_box.w;
		rect->h  = MAX(rect->h, bl_dim->subtree_box.h);

		h_space = opts->h_gap;
		v_space = opts->v_gap;
	}

	rect->w = MAX(rect->w, dims->box.w + opts->h_gap);

	dims->box.x = x + doz(rect->w, dims->box.w) / 2;
	dims->box.y = rect->h + v_space;

	rect->h = dims->box.y + dims->box.h;
}

static void set_y(const draw_chordal_env_t *env, ir_node *bl, int up)
{
	const draw_chordal_opts_t *opts      = env->opts;
	struct block_dims         *dims      = pmap_get(struct block_dims, env->block_dims, bl);
	int                       max_height = dims->subtree_box.h - dims->box.h - opts->v_gap;

	dominates_for_each(bl, sub) {
		struct block_dims *bl_dim = pmap_get(struct block_dims, env->block_dims, sub);
		int height_diff = max_height - bl_dim->subtree_box.h;

		set_y(env, sub, up + height_diff);
	}

	dims->subtree_box.y += up;
	dims->box.y         += up;
}

static color_t *reg_to_color(const draw_chordal_env_t *env,
							 ir_node *rel_bl, ir_node *irn, color_t *color)
{
	int phi_arg = 0;
	(void) env;
	(void) rel_bl;

	foreach_out_edge(irn, edge)
		phi_arg |= is_Phi(edge->src);

	color->r = is_Phi(irn) ? 0.5 : 0.0;
	color->g = phi_arg ? 0.5 : 0.0;
	color->b = 0.0;
	return color;
}

static void draw_block(ir_node *bl, void *data)
{
	static const color_t      black    = { 0, 0, 0 };
	const draw_chordal_env_t  *env     = (const draw_chordal_env_t*)data;
	const be_lv_t             *lv      = be_get_irg_liveness(env->chordal_env->irg);
	struct list_head          *head    = get_block_border_head(env->chordal_env, bl);
	ir_node                   *dom     = get_Block_idom(bl);
	const draw_chordal_opts_t *opts    = env->opts;
	struct block_dims         *dims    = pmap_get(struct block_dims, env->block_dims, bl);
	char                      buf[64];

	ir_snprintf(buf, sizeof(buf), "%F", bl);

	env->plotter->vtab->set_color(env->plotter, &black);
	env->plotter->vtab->box(env->plotter, &dims->box);

	env->plotter->vtab->text(env->plotter, dims->box.x, dims->box.y, buf);

	foreach_border_head(head, b) {
		if (b->is_def) {
			/* Walk from def to use, so the link is set before retrieved. */
			set_irn_link(b->irn, b);
		} else {
			ir_node               *const irn = b->irn;
			border_t        const *const def = (border_t const*)get_irn_link(irn);
			arch_register_t const *const reg = arch_get_irn_register(irn);

			int live_out = be_is_live_out(lv, bl, irn);
			int x        = (reg->index + 1) * opts->h_inter_gap;
			int ystart   = def->step * opts->v_inter_gap;
			int ystop    =   b->step * opts->v_inter_gap + (live_out ? 0 : opts->v_inter_gap / 2);

			color_t color;
			reg_to_color(env, bl, irn, &color);

			x      += dims->box.x;
			ystart += dims->box.y;
			ystop  += dims->box.y;

			env->plotter->vtab->set_color(env->plotter, &color);
			env->plotter->vtab->line(env->plotter, x, ystart, x, ystop);

			env->plotter->vtab->line(env->plotter, x - 2, ystart, x + 2, ystart);
			env->plotter->vtab->line(env->plotter, x - 2, ystop, x + 2, ystop);
		}
	}

	if (dom) {
		struct block_dims *dom_dims = pmap_get(struct block_dims, env->block_dims, dom);

		be_lv_foreach_cls(lv, bl, be_lv_state_in, env->cls, irn) {
			const arch_register_t *reg = arch_get_irn_register(irn);
			int                    x   = (reg->index + 1) * opts->h_inter_gap;
			color_t                color;

			reg_to_color(env, bl, irn, &color);

			env->plotter->vtab->set_color(env->plotter, &color);
			env->plotter->vtab->line(env->plotter,
				dims->box.x + x,
				dims->box.y + dims->box.h,
				dom_dims->box.x + x,
				dom_dims->box.y);
		}
	}
}

static void draw(draw_chordal_env_t *env, const rect_t *start_box)
{
	ir_graph  *irg = env->chordal_env->irg;
	plotter_t *p = env->plotter;
	rect_t bbox;

	bbox.x = bbox.y = 0;
	bbox.w = start_box->w + 2 * env->opts->x_margin;
	bbox.h = start_box->h + 2 * env->opts->y_margin;

	be_assure_live_sets(irg);
	be_assure_live_chk(irg);

	p->vtab->begin(p, &bbox);
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irg_block_walk_graph(irg, draw_block, NULL, env);
	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
	p->vtab->finish(p);
}

void draw_interval_tree(const draw_chordal_opts_t *opts,
                        const be_chordal_env_t *chordal_env, plotter_t *plotter)
{
	draw_chordal_env_t env;
	struct block_dims  *start_dims;
	ir_node            *start_block = get_irg_start_block(chordal_env->irg);

	env.opts        = opts;
	env.block_dims  = pmap_create();
	env.plotter     = plotter;
	env.cls         = chordal_env->cls;
	env.chordal_env = chordal_env;
	obstack_init(&env.obst);

	irg_block_walk_graph(chordal_env->irg, block_dims_walker, NULL, &env);
	layout(&env, start_block, opts->x_margin);
	set_y(&env, start_block, opts->y_margin);
	start_dims = pmap_get(struct block_dims, env.block_dims, start_block);
	draw(&env, &start_dims->subtree_box);

	pmap_destroy(env.block_dims);
	obstack_free(&env.obst, NULL);
}
