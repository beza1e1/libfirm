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
 * @brief       Backend node support for generic backend nodes.
 * @author      Sebastian Hack
 * @date        17.05.2005
 * @version     $Id$
 *
 * Backend node support for generic backend nodes.
 * This file provides Perm, Copy, Spill and Reload nodes.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "obst.h"
#include "set.h"
#include "pmap.h"
#include "util.h"
#include "debug.h"
#include "fourcc.h"
#include "offset.h"
#include "bitfiddle.h"
#include "raw_bitset.h"

#include "irop_t.h"
#include "irmode_t.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "irprintf.h"
#include "irgwalk.h"
#include "iropt_t.h"

#include "be_t.h"
#include "belive_t.h"
#include "besched_t.h"
#include "benode_t.h"
#include "bearch_t.h"

#include "beirgmod.h"

#define OUT_POS(x) (-((x) + 1))

#define get_irn_attr(irn) get_irn_generic_attr(irn)
#define get_irn_attr_const(irn) get_irn_generic_attr_const(irn)

typedef struct {
	arch_register_req_t req;
	arch_irn_flags_t    flags;
} be_req_t;

typedef struct {
	const arch_register_t *reg;
	be_req_t               req;
	be_req_t               in_req;
} be_reg_data_t;

/** The generic be nodes attribute type. */
typedef struct {
	be_reg_data_t *reg_data;
} be_node_attr_t;

/** The be_Return nodes attribute type. */
typedef struct {
	be_node_attr_t node_attr;     /**< base attributes of every be node. */
	int            num_ret_vals;  /**< number of return values */
	unsigned       pop;           /**< number of bytes that should be popped */
	int            emit_pop;      /**< if set, emit pop bytes, even if pop = 0 */
} be_return_attr_t;

/** The be_IncSP attribute type. */
typedef struct {
	be_node_attr_t node_attr;   /**< base attributes of every be node. */
	int            offset;      /**< The offset by which the stack shall be expanded/shrinked. */
	int            align;       /**< whether stack should be aligned after the
	                                 IncSP */
} be_incsp_attr_t;

/** The be_Frame attribute type. */
typedef struct {
	be_node_attr_t  node_attr;   /**< base attributes of every be node. */
	ir_entity      *ent;
	int             offset;
} be_frame_attr_t;

/** The be_Call attribute type. */
typedef struct {
	be_node_attr_t  node_attr;  /**< base attributes of every be node. */
	ir_entity      *ent;        /**< The called entity if this is a static call. */
	unsigned        pop;
	ir_type        *call_tp;    /**< The call type, copied from the original Call node. */
} be_call_attr_t;

typedef struct {
	be_node_attr_t   node_attr;  /**< base attributes of every be node. */
	ir_entity      **in_entities;
	ir_entity      **out_entities;
} be_memperm_attr_t;

ir_op *op_be_Spill;
ir_op *op_be_Reload;
ir_op *op_be_Perm;
ir_op *op_be_MemPerm;
ir_op *op_be_Copy;
ir_op *op_be_Keep;
ir_op *op_be_CopyKeep;
ir_op *op_be_Call;
ir_op *op_be_Return;
ir_op *op_be_IncSP;
ir_op *op_be_AddSP;
ir_op *op_be_SubSP;
ir_op *op_be_RegParams;
ir_op *op_be_FrameAddr;
ir_op *op_be_Barrier;
ir_op *op_be_Unwind;

static const ir_op_ops be_node_op_ops;

#define N   irop_flag_none
#define L   irop_flag_labeled
#define C   irop_flag_commutative
#define X   irop_flag_cfopcode
#define I   irop_flag_ip_cfopcode
#define F   irop_flag_fragile
#define Y   irop_flag_forking
#define H   irop_flag_highlevel
#define c   irop_flag_constlike
#define K   irop_flag_keep
#define M   irop_flag_uses_memory

static int be_reqs_equal(const be_req_t *req1, const be_req_t *req2)
{
	if(!reg_reqs_equal(&req1->req, &req2->req))
		return 0;
	if(req1->flags != req2->flags)
		return 0;

	return 1;
}

/**
 * Compare two be node attributes.
 *
 * @return zero if both attributes are identically
 */
static int _node_cmp_attr(const be_node_attr_t *a, const be_node_attr_t *b) {
	int i, len;

	if (ARR_LEN(a->reg_data) != ARR_LEN(b->reg_data))
		return 1;

	len = ARR_LEN(a->reg_data);
	for (i = 0; i < len; ++i) {
		if (a->reg_data[i].reg != b->reg_data[i].reg ||
				!be_reqs_equal(&a->reg_data[i].in_req, &b->reg_data[i].in_req) ||
			    !be_reqs_equal(&a->reg_data[i].req,    &b->reg_data[i].req))
			return 1;
	}

	return 0;
}

/**
 * Compare the node attributes of two be_node's.
 *
 * @return zero if both nodes have identically attributes
 */
static int node_cmp_attr(ir_node *a, ir_node *b) {
	const be_node_attr_t *a_attr = get_irn_attr_const(a);
	const be_node_attr_t *b_attr = get_irn_attr_const(b);

	return _node_cmp_attr(a_attr, b_attr);
}

/**
 * Compare the attributes of two be_FrameAddr nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int FrameAddr_cmp_attr(ir_node *a, ir_node *b) {
	const be_frame_attr_t *a_attr = get_irn_attr_const(a);
	const be_frame_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->ent != b_attr->ent || a_attr->offset != b_attr->offset)
		return 1;

	return _node_cmp_attr(&a_attr->node_attr, &b_attr->node_attr);
}

/**
 * Compare the attributes of two be_Return nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int Return_cmp_attr(ir_node *a, ir_node *b) {
	const be_return_attr_t *a_attr = get_irn_attr_const(a);
	const be_return_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->num_ret_vals != b_attr->num_ret_vals)
		return 1;
	if (a_attr->pop != b_attr->pop)
		return 1;
	if (a_attr->emit_pop != b_attr->emit_pop)
		return 1;

	return _node_cmp_attr(&a_attr->node_attr, &b_attr->node_attr);
}

/**
 * Compare the attributes of two be_IncSP nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int IncSP_cmp_attr(ir_node *a, ir_node *b) {
	const be_incsp_attr_t *a_attr = get_irn_attr_const(a);
	const be_incsp_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->offset != b_attr->offset)
		return 1;

	return _node_cmp_attr(&a_attr->node_attr, &b_attr->node_attr);
}

/**
 * Compare the attributes of two be_Call nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int Call_cmp_attr(ir_node *a, ir_node *b) {
	const be_call_attr_t *a_attr = get_irn_attr_const(a);
	const be_call_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->ent != b_attr->ent ||
			a_attr->call_tp != b_attr->call_tp)
		return 1;

	return _node_cmp_attr(&a_attr->node_attr, &b_attr->node_attr);
}

static INLINE be_req_t *get_be_req(const ir_node *node, int pos)
{
	int idx;
	const be_node_attr_t *attr;
	be_reg_data_t *rd;

	assert(is_be_node(node));
	attr = get_irn_attr_const(node);

	if(pos < 0) {
		idx = -(pos + 1);
	} else {
		idx = pos;
		assert(idx < get_irn_arity(node));
	}
	assert(idx < ARR_LEN(attr->reg_data));
	rd = &attr->reg_data[idx];

	return pos < 0 ? &rd->req : &rd->in_req;
}

static INLINE arch_register_req_t *get_req(const ir_node *node, int pos)
{
	be_req_t *bereq = get_be_req(node, pos);
	return &bereq->req;
}

/**
 * Initializes the generic attribute of all be nodes and return ir.
 */
static void *init_node_attr(ir_node *node, int max_reg_data)
{
	ir_graph *irg = get_irn_irg(node);
	struct obstack *obst = get_irg_obstack(irg);
	be_node_attr_t *a = get_irn_attr(node);

	memset(a, 0, sizeof(get_op_attr_size(get_irn_op(node))));

	if(max_reg_data >= 0) {
		a->reg_data = NEW_ARR_D(be_reg_data_t, obst, max_reg_data);
		memset(a->reg_data, 0, max_reg_data * sizeof(a->reg_data[0]));
	} else {
		a->reg_data = NEW_ARR_F(be_reg_data_t, 0);
	}

	return a;
}

static void add_register_req(ir_node *node)
{
	be_node_attr_t *a = get_irn_attr(node);
	be_reg_data_t regreq;
	memset(&regreq, 0, sizeof(regreq));
	ARR_APP1(be_reg_data_t, a->reg_data, regreq);
}

/**
 * Skip Proj nodes and return their Proj numbers.
 *
 * If *node is a Proj or Proj(Proj) node, skip it.
 *
 * @param node  points to the node to be skipped
 *
 * @return 0 if *node was no Proj node, its Proj number else.
 */
static int redir_proj(const ir_node **node)
{
	const ir_node *n = *node;

	if(is_Proj(n)) {
		ir_node *irn;

		*node = irn = get_Proj_pred(n);
		if(is_Proj(irn)) {
			assert(get_irn_mode(irn) == mode_T);
			*node = get_Proj_pred(irn);
		}
		return get_Proj_proj(n);
	}

	return 0;
}

static be_reg_data_t *retrieve_reg_data(const ir_node *node)
{
	const be_node_attr_t *attr;
	int pos = 0;

	if(is_Proj(node)) {
		pos = get_Proj_proj(node);
		node = get_Proj_pred(node);
	}

	assert(is_be_node(node));
	attr = get_irn_attr_const(node);
	assert(pos >= 0 && pos < ARR_LEN(attr->reg_data) && "illegal proj number");

	return &attr->reg_data[pos];
}

static void
be_node_set_irn_reg(ir_node *irn, const arch_register_t *reg)
{
	be_reg_data_t *r = retrieve_reg_data(irn);
	r->reg = reg;
}

ir_node *be_new_Spill(const arch_register_class_t *cls, const arch_register_class_t *cls_frame,
	ir_graph *irg, ir_node *bl, ir_node *frame, ir_node *to_spill)
{
	be_frame_attr_t *a;
	ir_node         *in[2];
	ir_node         *res;

	in[0]     = frame;
	in[1]     = to_spill;
	res       = new_ir_node(NULL, irg, bl, op_be_Spill, mode_M, 2, in);
	a         = init_node_attr(res, 2);
	a->ent    = NULL;
	a->offset = 0;

	be_node_set_reg_class(res, be_pos_Spill_frame, cls_frame);
	be_node_set_reg_class(res, be_pos_Spill_val, cls);
	return res;
}

ir_node *be_new_Reload(const arch_register_class_t *cls, const arch_register_class_t *cls_frame,
	ir_graph *irg, ir_node *bl, ir_node *frame, ir_node *mem, ir_mode *mode)
{
	ir_node *in[2];
	ir_node *res;

	in[0] = frame;
	in[1] = mem;
	res   = new_ir_node(NULL, irg, bl, op_be_Reload, mode, 2, in);

	init_node_attr(res, 2);
	be_node_set_reg_class(res, -1, cls);
	be_node_set_reg_class(res, be_pos_Reload_frame, cls_frame);
	be_node_set_flags(res, -1, arch_irn_flags_rematerializable);
	return res;
}

ir_node *be_get_Reload_mem(const ir_node *irn)
{
	assert(be_is_Reload(irn));
	return get_irn_n(irn, be_pos_Reload_mem);
}

ir_node *be_get_Reload_frame(const ir_node *irn)
{
	assert(be_is_Reload(irn));
	return get_irn_n(irn, be_pos_Reload_frame);
}

ir_node *be_get_Spill_val(const ir_node *irn)
{
	assert(be_is_Spill(irn));
	return get_irn_n(irn, be_pos_Spill_val);
}

ir_node *be_get_Spill_frame(const ir_node *irn)
{
	assert(be_is_Spill(irn));
	return get_irn_n(irn, be_pos_Spill_frame);
}

ir_node *be_new_Perm(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, int n, ir_node *in[])
{
	int i;
	ir_node *irn = new_ir_node(NULL, irg, bl, op_be_Perm, mode_T, n, in);
	init_node_attr(irn, n);
	for(i = 0; i < n; ++i) {
		be_node_set_reg_class(irn, i, cls);
		be_node_set_reg_class(irn, OUT_POS(i), cls);
	}

	return irn;
}

void be_Perm_reduce(ir_node *perm, int new_size, int *map)
{
	ir_graph *irg           = get_irn_irg(perm);
	int            arity    = get_irn_arity(perm);
	be_reg_data_t *old_data = alloca(arity * sizeof(old_data[0]));
	be_node_attr_t *attr    = get_irn_attr(perm);
	ir_node **new_in        = NEW_ARR_D(ir_node *, irg->obst, new_size);

	int i;

	assert(be_is_Perm(perm));
	assert(new_size <= arity);

	/* save the old register data */
	memcpy(old_data, attr->reg_data, arity * sizeof(old_data[0]));

	/* compose the new in array and set the new register data directly in place */
	for (i = 0; i < new_size; ++i) {
		int idx = map[i];
		new_in[i]         = get_irn_n(perm, idx);
		attr->reg_data[i] = old_data[idx];
	}

	set_irn_in(perm, new_size, new_in);
}

ir_node *be_new_MemPerm(const arch_env_t *arch_env, ir_graph *irg, ir_node *bl, int n, ir_node *in[])
{
	int i;
	ir_node *frame = get_irg_frame(irg);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class(arch_env, frame, -1);
	ir_node *irn;
	const arch_register_t *sp = arch_env->sp;
	be_memperm_attr_t *attr;
	ir_node **real_in;

	real_in = alloca((n+1) * sizeof(real_in[0]));
	real_in[0] = frame;
	memcpy(&real_in[1], in, n * sizeof(real_in[0]));

	irn =  new_ir_node(NULL, irg, bl, op_be_MemPerm, mode_T, n+1, real_in);

	init_node_attr(irn, n + 1);
	be_node_set_reg_class(irn, 0, sp->reg_class);
	for (i = 0; i < n; ++i) {
		be_node_set_reg_class(irn, i + 1, cls_frame);
		be_node_set_reg_class(irn, OUT_POS(i), cls_frame);
	}

	attr = get_irn_attr(irn);

	attr->in_entities = obstack_alloc(irg->obst, n * sizeof(attr->in_entities[0]));
	memset(attr->in_entities, 0, n * sizeof(attr->in_entities[0]));
	attr->out_entities = obstack_alloc(irg->obst, n*sizeof(attr->out_entities[0]));
	memset(attr->out_entities, 0, n*sizeof(attr->out_entities[0]));

	return irn;
}


ir_node *be_new_Copy(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, ir_node *op)
{
	ir_node *in[1];
	ir_node *res;
	arch_register_req_t *req;

	in[0] = op;
	res   = new_ir_node(NULL, irg, bl, op_be_Copy, get_irn_mode(op), 1, in);
	init_node_attr(res, 1);
	be_node_set_reg_class(res, 0, cls);
	be_node_set_reg_class(res, OUT_POS(0), cls);

	req = get_req(res, OUT_POS(0));
	req->cls = cls;
	req->type = arch_register_req_type_should_be_same;
	req->other_same = 1U << 0;

	return res;
}

ir_node *be_get_Copy_op(const ir_node *cpy) {
	return get_irn_n(cpy, be_pos_Copy_op);
}

void be_set_Copy_op(ir_node *cpy, ir_node *op) {
	set_irn_n(cpy, be_pos_Copy_op, op);
}

ir_node *be_new_Keep(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, int n, ir_node *in[])
{
	int i;
	ir_node *res;

	res = new_ir_node(NULL, irg, bl, op_be_Keep, mode_ANY, -1, NULL);
	init_node_attr(res, -1);

	for(i = 0; i < n; ++i) {
		add_irn_n(res, in[i]);
		add_register_req(res);
		be_node_set_reg_class(res, i, cls);
	}
	keep_alive(res);

	return res;
}

void be_Keep_add_node(ir_node *keep, const arch_register_class_t *cls, ir_node *node)
{
	int n;

	assert(be_is_Keep(keep));
	n = add_irn_n(keep, node);
	add_register_req(keep);
	be_node_set_reg_class(keep, n, cls);
}

/* creates a be_Call */
ir_node *be_new_Call(dbg_info *dbg, ir_graph *irg, ir_node *bl, ir_node *mem, ir_node *sp, ir_node *ptr,
                     int n_outs, int n, ir_node *in[], ir_type *call_tp)
{
	be_call_attr_t *a;
	int real_n = be_pos_Call_first_arg + n;
	ir_node *irn;
	ir_node **real_in;

	NEW_ARR_A(ir_node *, real_in, real_n);
	real_in[be_pos_Call_mem] = mem;
	real_in[be_pos_Call_sp]  = sp;
	real_in[be_pos_Call_ptr] = ptr;
	memcpy(&real_in[be_pos_Call_first_arg], in, n * sizeof(in[0]));

	irn = new_ir_node(dbg, irg, bl, op_be_Call, mode_T, real_n, real_in);
	a = init_node_attr(irn, (n_outs > real_n ? n_outs : real_n));
	a->ent     = NULL;
	a->call_tp = call_tp;
	a->pop     = 0;
	return irn;
}

/* Gets the call entity or NULL if this is no static call. */
ir_entity *be_Call_get_entity(const ir_node *call) {
	const be_call_attr_t *a = get_irn_attr_const(call);
	assert(be_is_Call(call));
	return a->ent;
}

/* Sets the call entity. */
void be_Call_set_entity(ir_node *call, ir_entity *ent) {
	be_call_attr_t *a = get_irn_attr(call);
	assert(be_is_Call(call));
	a->ent = ent;
}

/* Gets the call type. */
ir_type *be_Call_get_type(ir_node *call) {
	const be_call_attr_t *a = get_irn_attr_const(call);
	assert(be_is_Call(call));
	return a->call_tp;
}

/* Sets the call type. */
void be_Call_set_type(ir_node *call, ir_type *call_tp) {
	be_call_attr_t *a = get_irn_attr(call);
	assert(be_is_Call(call));
	a->call_tp = call_tp;
}

void be_Call_set_pop(ir_node *call, unsigned pop) {
	be_call_attr_t *a = get_irn_attr(call);
	a->pop = pop;
}

unsigned be_Call_get_pop(const ir_node *call) {
	const be_call_attr_t *a = get_irn_attr_const(call);
	return a->pop;
}

/* Construct a new be_Return. */
ir_node *be_new_Return(dbg_info *dbg, ir_graph *irg, ir_node *block, int n_res,
                       unsigned pop, int n, ir_node *in[])
{
	be_return_attr_t *a;
	ir_node *res;
	int i;

	res = new_ir_node(dbg, irg, block, op_be_Return, mode_X, -1, NULL);
	init_node_attr(res, -1);
	for(i = 0; i < n; ++i) {
		add_irn_n(res, in[i]);
		add_register_req(res);
	}

	a = get_irn_attr(res);
	a->num_ret_vals = n_res;
	a->pop          = pop;
	a->emit_pop     = 0;

	return res;
}

/* Returns the number of real returns values */
int be_Return_get_n_rets(const ir_node *ret) {
	const be_return_attr_t *a = get_irn_generic_attr_const(ret);
	return a->num_ret_vals;
}

/* return the number of bytes that should be popped from stack when executing the Return. */
unsigned be_Return_get_pop(const ir_node *ret) {
	const be_return_attr_t *a = get_irn_generic_attr_const(ret);
	return a->pop;
}

/* return non-zero, if number of popped bytes must be always emitted */
int be_Return_get_emit_pop(const ir_node *ret) {
	const be_return_attr_t *a = get_irn_generic_attr_const(ret);
	return a->emit_pop;
}

/* return non-zero, if number of popped bytes must be always emitted */
void be_Return_set_emit_pop(ir_node *ret, int emit_pop) {
	be_return_attr_t *a = get_irn_generic_attr(ret);
	a->emit_pop = emit_pop;
}

int be_Return_append_node(ir_node *ret, ir_node *node) {
	int pos;

	pos = add_irn_n(ret, node);
	add_register_req(ret);

	return pos;
}

ir_node *be_new_IncSP(const arch_register_t *sp, ir_graph *irg, ir_node *bl,
                      ir_node *old_sp, int offset, int align)
{
	be_incsp_attr_t *a;
	ir_node *irn;
	ir_node *in[1];

	in[0]     = old_sp;
	irn       = new_ir_node(NULL, irg, bl, op_be_IncSP, sp->reg_class->mode,
	                        sizeof(in) / sizeof(in[0]), in);
	a         = init_node_attr(irn, 1);
	a->offset = offset;
	a->align  = align;

	be_node_set_flags(irn, -1, arch_irn_flags_ignore | arch_irn_flags_modify_sp);

	/* Set output constraint to stack register. */
	be_node_set_reg_class(irn, 0, sp->reg_class);
	be_set_constr_single_reg(irn, BE_OUT_POS(0), sp);
	be_node_set_irn_reg(irn, sp);

	return irn;
}

ir_node *be_new_AddSP(const arch_register_t *sp, ir_graph *irg, ir_node *bl, ir_node *old_sp, ir_node *sz)
{
	be_node_attr_t *a;
	ir_node *irn;
	ir_node *in[be_pos_AddSP_last];
	const arch_register_class_t *class;

	in[be_pos_AddSP_old_sp] = old_sp;
	in[be_pos_AddSP_size]   = sz;

	irn = new_ir_node(NULL, irg, bl, op_be_AddSP, mode_T, be_pos_AddSP_last, in);
	a   = init_node_attr(irn, be_pos_AddSP_last);

	be_node_set_flags(irn, OUT_POS(pn_be_AddSP_sp),
	                  arch_irn_flags_ignore | arch_irn_flags_modify_sp);

	/* Set output constraint to stack register. */
	be_set_constr_single_reg(irn, be_pos_AddSP_old_sp, sp);
	be_node_set_reg_class(irn, be_pos_AddSP_size, arch_register_get_class(sp));
	be_set_constr_single_reg(irn, OUT_POS(pn_be_AddSP_sp), sp);
	a->reg_data[pn_be_AddSP_sp].reg = sp;

	class = arch_register_get_class(sp);
	be_node_set_reg_class(irn, OUT_POS(pn_be_AddSP_res), class);

	return irn;
}

ir_node *be_new_SubSP(const arch_register_t *sp, ir_graph *irg, ir_node *bl, ir_node *old_sp, ir_node *sz)
{
	be_node_attr_t *a;
	ir_node *irn;
	ir_node *in[be_pos_SubSP_last];

	in[be_pos_SubSP_old_sp] = old_sp;
	in[be_pos_SubSP_size]   = sz;

	irn = new_ir_node(NULL, irg, bl, op_be_SubSP, mode_T, be_pos_SubSP_last, in);
	a   = init_node_attr(irn, be_pos_SubSP_last);

	be_node_set_flags(irn, OUT_POS(pn_be_SubSP_sp),
	                  arch_irn_flags_ignore | arch_irn_flags_modify_sp);

	/* Set output constraint to stack register. */
	be_set_constr_single_reg(irn, be_pos_SubSP_old_sp, sp);
	be_node_set_reg_class(irn, be_pos_SubSP_size, arch_register_get_class(sp));
	be_set_constr_single_reg(irn, OUT_POS(pn_be_SubSP_sp), sp);
	a->reg_data[pn_be_SubSP_sp].reg = sp;

	return irn;
}

ir_node *be_new_RegParams(ir_graph *irg, ir_node *bl, int n_outs)
{
	ir_node *res;
	int i;

	res = new_ir_node(NULL, irg, bl, op_be_RegParams, mode_T, 0, NULL);
	init_node_attr(res, -1);
	for(i = 0; i < n_outs; ++i)
		add_register_req(res);

	return res;
}

ir_node *be_RegParams_append_out_reg(ir_node *regparams,
                                     const arch_env_t *arch_env,
                                     const arch_register_t *reg)
{
	ir_graph *irg = get_irn_irg(regparams);
	ir_node *block = get_nodes_block(regparams);
	be_node_attr_t *attr = get_irn_attr(regparams);
	const arch_register_class_t *cls = arch_register_get_class(reg);
	ir_mode *mode = arch_register_class_mode(cls);
	int n = ARR_LEN(attr->reg_data);
	ir_node *proj;

	assert(be_is_RegParams(regparams));
	proj = new_r_Proj(irg, block, regparams, mode, n);
	add_register_req(regparams);
	be_set_constr_single_reg(regparams, BE_OUT_POS(n), reg);
	arch_set_irn_register(arch_env, proj, reg);

	/* TODO decide, whether we need to set ignore/modify sp flags here? */

	return proj;
}

ir_node *be_new_FrameAddr(const arch_register_class_t *cls_frame, ir_graph *irg, ir_node *bl, ir_node *frame, ir_entity *ent)
{
	be_frame_attr_t *a;
	ir_node *irn;
	ir_node *in[1];

	in[0]  = frame;
	irn    = new_ir_node(NULL, irg, bl, op_be_FrameAddr, get_irn_mode(frame), 1, in);
	a      = init_node_attr(irn, 1);
	a->ent = ent;
	a->offset = 0;
	be_node_set_reg_class(irn, 0, cls_frame);
	be_node_set_reg_class(irn, OUT_POS(0), cls_frame);

	return optimize_node(irn);
}

ir_node *be_get_FrameAddr_frame(const ir_node *node) {
	assert(be_is_FrameAddr(node));
	return get_irn_n(node, be_pos_FrameAddr_ptr);
}

ir_entity *be_get_FrameAddr_entity(const ir_node *node)
{
	const be_frame_attr_t *attr = get_irn_generic_attr_const(node);
	return attr->ent;
}

ir_node *be_new_CopyKeep(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, ir_node *src, int n, ir_node *in_keep[], ir_mode *mode)
{
	ir_node *irn;
	ir_node **in = (ir_node **) alloca((n + 1) * sizeof(in[0]));

	in[0] = src;
	memcpy(&in[1], in_keep, n * sizeof(in[0]));
	irn   = new_ir_node(NULL, irg, bl, op_be_CopyKeep, mode, n + 1, in);
	init_node_attr(irn, n + 1);
	be_node_set_reg_class(irn, OUT_POS(0), cls);
	be_node_set_reg_class(irn, 0, cls);

	return irn;
}

ir_node *be_new_CopyKeep_single(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, ir_node *src, ir_node *keep, ir_mode *mode)
{
	return be_new_CopyKeep(cls, irg, bl, src, 1, &keep, mode);
}

ir_node *be_get_CopyKeep_op(const ir_node *cpy) {
	return get_irn_n(cpy, be_pos_CopyKeep_op);
}

void be_set_CopyKeep_op(ir_node *cpy, ir_node *op) {
	set_irn_n(cpy, be_pos_CopyKeep_op, op);
}

ir_node *be_new_Barrier(ir_graph *irg, ir_node *bl, int n, ir_node *in[])
{
	ir_node *res;
	int i;

	res = new_ir_node(NULL, irg, bl, op_be_Barrier, mode_T, -1, NULL);
	init_node_attr(res, -1);
	for(i = 0; i < n; ++i) {
		add_irn_n(res, in[i]);
		add_register_req(res);
	}

	return res;
}

ir_node *be_Barrier_append_node(ir_node *barrier, ir_node *node)
{
	ir_graph *irg = get_irn_irg(barrier);
	ir_node *block = get_nodes_block(barrier);
	ir_mode *mode = get_irn_mode(node);
	int n = add_irn_n(barrier, node);

	ir_node *proj = new_r_Proj(irg, block, barrier, mode, n);
	add_register_req(barrier);

	return proj;
}

/* Construct a new be_Unwind. */
ir_node *be_new_Unwind(dbg_info *dbg, ir_graph *irg, ir_node *block,
					   ir_node *mem, ir_node *sp)
{
	ir_node *res;
	ir_node *in[2];

	in[be_pos_Unwind_mem] = mem;
	in[be_pos_Unwind_sp]  = sp;
	res = new_ir_node(dbg, irg, block, op_be_Unwind, mode_X, 2, in);
	init_node_attr(res, -1);

	return res;
}

int be_has_frame_entity(const ir_node *irn)
{
	switch (get_irn_opcode(irn)) {
	case beo_Spill:
	case beo_Reload:
	case beo_FrameAddr:
		return 1;
	default:
		return 0;
	}
}

ir_entity *be_get_frame_entity(const ir_node *irn)
{
	if (be_has_frame_entity(irn)) {
		const be_frame_attr_t *a = get_irn_attr_const(irn);
		return a->ent;
	}
	return NULL;
}

int be_get_frame_offset(const ir_node *irn)
{
	assert(is_be_node(irn));
	if (be_has_frame_entity(irn)) {
		const be_frame_attr_t *a = get_irn_attr_const(irn);
		return a->offset;
	}
	return 0;
}

void be_set_MemPerm_in_entity(const ir_node *irn, int n, ir_entity *ent)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	attr->in_entities[n] = ent;
}

ir_entity* be_get_MemPerm_in_entity(const ir_node* irn, int n)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	return attr->in_entities[n];
}

void be_set_MemPerm_out_entity(const ir_node *irn, int n, ir_entity *ent)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	attr->out_entities[n] = ent;
}

ir_entity* be_get_MemPerm_out_entity(const ir_node* irn, int n)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	return attr->out_entities[n];
}

int be_get_MemPerm_entity_arity(const ir_node *irn)
{
	return get_irn_arity(irn) - 1;
}

void be_set_constr_single_reg(ir_node *node, int pos, const arch_register_t *reg)
{
	arch_register_req_t *req = get_req(node, pos);
	const arch_register_class_t *cls = arch_register_get_class(reg);
	ir_graph *irg = get_irn_irg(node);
	struct obstack *obst = get_irg_obstack(irg);
	unsigned *limited_bitset;

	assert(req->cls == NULL || req->cls == cls);
	assert(! (req->type & arch_register_req_type_limited));
	assert(req->limited == NULL);

	limited_bitset = rbitset_obstack_alloc(obst, arch_register_class_n_regs(cls));
	rbitset_set(limited_bitset, arch_register_get_index(reg));

	req->cls = cls;
	req->type |= arch_register_req_type_limited;
	req->limited = limited_bitset;
}

void be_set_constr_limited(ir_node *node, int pos, const arch_register_req_t *req)
{
	ir_graph *irg = get_irn_irg(node);
	struct obstack *obst = get_irg_obstack(irg);
	arch_register_req_t *r = get_req(node, pos);

	assert(arch_register_req_is(req, limited));
	assert(! (req->type & (arch_register_req_type_should_be_same | arch_register_req_type_should_be_different)));
	memcpy(r, req, sizeof(r[0]));
	r->limited = rbitset_duplicate_obstack_alloc(obst, req->limited, req->cls->n_regs);
}

void be_node_set_flags(ir_node *irn, int pos, arch_irn_flags_t flags)
{
	be_req_t *bereq = get_be_req(irn, pos);
	bereq->flags = flags;
}

void be_node_add_flags(ir_node *irn, int pos, arch_irn_flags_t flags)
{
	be_req_t *bereq = get_be_req(irn, pos);
	bereq->flags |= flags;
}

void be_node_set_reg_class(ir_node *irn, int pos, const arch_register_class_t *cls)
{
	arch_register_req_t *req = get_req(irn, pos);

	req->cls = cls;

	if (cls == NULL) {
		req->type = arch_register_req_type_none;
	} else if (req->type == arch_register_req_type_none) {
		req->type = arch_register_req_type_normal;
	}
}

void be_node_set_req_type(ir_node *irn, int pos, arch_register_req_type_t type)
{
	arch_register_req_t *req = get_req(irn, pos);
	req->type = type;
}

ir_node *be_get_IncSP_pred(ir_node *irn) {
	assert(be_is_IncSP(irn));
	return get_irn_n(irn, 0);
}

void be_set_IncSP_pred(ir_node *incsp, ir_node *pred) {
	assert(be_is_IncSP(incsp));
	set_irn_n(incsp, 0, pred);
}

void be_set_IncSP_offset(ir_node *irn, int offset)
{
	be_incsp_attr_t *a = get_irn_attr(irn);
	assert(be_is_IncSP(irn));
	a->offset = offset;
}

int be_get_IncSP_offset(const ir_node *irn)
{
	const be_incsp_attr_t *a = get_irn_attr_const(irn);
	assert(be_is_IncSP(irn));
	return a->offset;
}

int be_get_IncSP_align(const ir_node *irn)
{
	const be_incsp_attr_t *a = get_irn_attr_const(irn);
	assert(be_is_IncSP(irn));
	return a->align;
}

ir_node *be_spill(const arch_env_t *arch_env, ir_node *block, ir_node *irn)
{
	ir_graph                    *irg       = get_irn_irg(block);
	ir_node                     *frame     = get_irg_frame(irg);
	const arch_register_class_t *cls       = arch_get_irn_reg_class(arch_env, irn, -1);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class(arch_env, frame, -1);
	ir_node                     *spill;

	spill = be_new_Spill(cls, cls_frame, irg, block, frame, irn);
	return spill;
}

ir_node *be_reload(const arch_env_t *arch_env, const arch_register_class_t *cls, ir_node *insert, ir_mode *mode, ir_node *spill)
{
	ir_node  *reload;
	ir_node  *bl    = is_Block(insert) ? insert : get_nodes_block(insert);
	ir_graph *irg   = get_irn_irg(bl);
	ir_node  *frame = get_irg_frame(irg);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class(arch_env, frame, -1);

	assert(be_is_Spill(spill) || (is_Phi(spill) && get_irn_mode(spill) == mode_M));

	reload = be_new_Reload(cls, cls_frame, irg, bl, frame, spill, mode);

	if (is_Block(insert)) {
		insert = sched_skip(insert, 0, sched_skip_cf_predicator, (void *) arch_env);
		sched_add_after(insert, reload);
	} else {
		sched_add_before(insert, reload);
	}

	return reload;
}

/*
  ____              ____
 |  _ \ ___  __ _  |  _ \ ___  __ _ ___
 | |_) / _ \/ _` | | |_) / _ \/ _` / __|
 |  _ <  __/ (_| | |  _ <  __/ (_| \__ \
 |_| \_\___|\__, | |_| \_\___|\__, |___/
            |___/                |_|

*/


static const
arch_register_req_t *get_out_reg_req(const ir_node *irn, int out_pos)
{
	const be_node_attr_t *a = get_irn_attr_const(irn);

	if(out_pos >= ARR_LEN(a->reg_data)) {
		return arch_no_register_req;
	}

	return &a->reg_data[out_pos].req.req;
}

static const
arch_register_req_t *get_in_reg_req(const ir_node *irn, int pos)
{
	const be_node_attr_t *a = get_irn_attr_const(irn);

	if(pos >= get_irn_arity(irn) || pos >= ARR_LEN(a->reg_data))
		return arch_no_register_req;

	return &a->reg_data[pos].in_req.req;
}

static const arch_register_req_t *
be_node_get_irn_reg_req(const ir_node *irn, int pos)
{
	int out_pos = pos;

	if (pos < 0) {
		if (get_irn_mode(irn) == mode_T)
			return arch_no_register_req;

		out_pos = redir_proj((const ir_node **)&irn);
		assert(is_be_node(irn));
		return get_out_reg_req(irn, out_pos);
	} else if (is_be_node(irn)) {
		/*
   		 * For spills and reloads, we return "none" as requirement for frame
		 * pointer, so every input is ok. Some backends need this (e.g. STA).
		 */
		if ((be_is_Spill(irn)  && pos == be_pos_Spill_frame) ||
				(be_is_Reload(irn) && pos == be_pos_Reload_frame))
			return arch_no_register_req;

		return get_in_reg_req(irn, pos);
	}

	return arch_no_register_req;
}

const arch_register_t *
be_node_get_irn_reg(const ir_node *irn)
{
	be_reg_data_t *r;

	if (get_irn_mode(irn) == mode_T)
		return NULL;
	r = retrieve_reg_data(irn);
	return r->reg;
}

static arch_irn_class_t be_node_classify(const ir_node *irn)
{
restart:
	switch (get_irn_opcode(irn)) {
#define XXX(a,b) case a: return b
		XXX(beo_Spill, arch_irn_class_spill);
		XXX(beo_Reload, arch_irn_class_reload);
		XXX(beo_Perm, arch_irn_class_perm);
		XXX(beo_Copy, arch_irn_class_copy);
		XXX(beo_Return, arch_irn_class_branch);
#undef XXX
		case iro_Proj:
			irn = get_Proj_pred(irn);
			if (is_Proj(irn)) {
				assert(get_irn_mode(irn) == mode_T);
				irn = get_Proj_pred(irn);
			}
			goto restart;
			break;
		default:
			return arch_irn_class_normal;
	}

	return 0;
}

static arch_irn_flags_t be_node_get_flags(const ir_node *node)
{
	be_req_t *bereq;
	int pos = -1;

	if(is_Proj(node)) {
		pos = OUT_POS(get_Proj_proj(node));
		node = skip_Proj_const(node);
	}

	bereq = get_be_req(node, pos);

	return bereq->flags;
}

static ir_entity *be_node_get_frame_entity(const ir_node *irn)
{
	return be_get_frame_entity(irn);
}

static void be_node_set_frame_entity(ir_node *irn, ir_entity *ent)
{
	be_frame_attr_t *a;

	assert(be_has_frame_entity(irn));

	a = get_irn_attr(irn);
	a->ent = ent;
}

static void be_node_set_frame_offset(ir_node *irn, int offset)
{
	if(be_has_frame_entity(irn)) {
		be_frame_attr_t *a = get_irn_attr(irn);
		a->offset = offset;
	}
}

static int be_node_get_sp_bias(const ir_node *irn)
{
	if(be_is_IncSP(irn))
		return be_get_IncSP_offset(irn);
	if(be_is_Call(irn))
		return -(int)be_Call_get_pop(irn);

	return 0;
}

/*
  ___ ____  _   _   _   _                 _ _
 |_ _|  _ \| \ | | | | | | __ _ _ __   __| | | ___ _ __
  | || |_) |  \| | | |_| |/ _` | '_ \ / _` | |/ _ \ '__|
  | ||  _ <| |\  | |  _  | (_| | | | | (_| | |  __/ |
 |___|_| \_\_| \_| |_| |_|\__,_|_| |_|\__,_|_|\___|_|

*/

static const arch_irn_ops_t be_node_irn_ops = {
	be_node_get_irn_reg_req,
	be_node_set_irn_reg,
	be_node_get_irn_reg,
	be_node_classify,
	be_node_get_flags,
	be_node_get_frame_entity,
	be_node_set_frame_entity,
	be_node_set_frame_offset,
	be_node_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

/*
  ____  _     _   ___ ____  _   _   _   _                 _ _
 |  _ \| |__ (_) |_ _|  _ \| \ | | | | | | __ _ _ __   __| | | ___ _ __
 | |_) | '_ \| |  | || |_) |  \| | | |_| |/ _` | '_ \ / _` | |/ _ \ '__|
 |  __/| | | | |  | ||  _ <| |\  | |  _  | (_| | | | | (_| | |  __/ |
 |_|   |_| |_|_| |___|_| \_\_| \_| |_| |_|\__,_|_| |_|\__,_|_|\___|_|

*/

typedef struct {
	const arch_register_t *reg;
	arch_register_req_t    req;
	arch_irn_flags_t       flags;
} phi_attr_t;

struct {
	arch_env_t  *arch_env;
	pmap        *phi_attrs;
} phi_handler;

#define get_phi_handler_from_ops(h)      container_of(h, phi_handler_t, irn_ops)

static INLINE
phi_attr_t *get_Phi_attr(const ir_node *phi)
{
	phi_attr_t *attr = pmap_get(phi_handler.phi_attrs, (void*) phi);
	if(attr == NULL) {
		ir_graph *irg = get_irn_irg(phi);
		struct obstack *obst = get_irg_obstack(irg);
		attr = obstack_alloc(obst, sizeof(attr[0]));
		memset(attr, 0, sizeof(attr[0]));
		pmap_insert(phi_handler.phi_attrs, phi, attr);
	}

	return attr;
}

/**
 * Get register class of a Phi.
 */
static
const arch_register_req_t *get_Phi_reg_req_recursive(const ir_node *phi,
                                                     pset **visited)
{
	int n = get_irn_arity(phi);
	ir_node *op;
	int i;

	if(*visited && pset_find_ptr(*visited, phi))
		return NULL;

	for(i = 0; i < n; ++i) {
		op = get_irn_n(phi, i);
		/* Matze: don't we unnecessary constraint our phis with this?
		 * we only need to take the regclass IMO*/
		if(!is_Phi(op))
			return arch_get_register_req(phi_handler.arch_env, op, BE_OUT_POS(0));
	}

	/*
	 * The operands of that Phi were all Phis themselves.
	 * We have to start a DFS for a non-Phi argument now.
	 */
	if(!*visited)
		*visited = pset_new_ptr(16);

	pset_insert_ptr(*visited, phi);

	for(i = 0; i < n; ++i) {
		const arch_register_req_t *req;
		op = get_irn_n(phi, i);
		req = get_Phi_reg_req_recursive(op, visited);
		if(req != NULL)
			return req;
	}

	return NULL;
}

static
const arch_register_req_t *phi_get_irn_reg_req(const ir_node *irn, int pos)
{
	phi_attr_t *attr;
	(void) pos;

	if(!mode_is_datab(get_irn_mode(irn)))
		return arch_no_register_req;

	attr = get_Phi_attr(irn);

	if(attr->req.type == arch_register_req_type_none) {
		pset *visited = NULL;
		const arch_register_req_t *req;
		req = get_Phi_reg_req_recursive(irn, &visited);

		memcpy(&attr->req, req, sizeof(req[0]));
		assert(attr->req.cls != NULL);
		attr->req.type = arch_register_req_type_normal;

		if(visited != NULL)
			del_pset(visited);
	}

	return &attr->req;
}

void be_set_phi_reg_req(const arch_env_t *arch_env, ir_node *node,
                        const arch_register_req_t *req)
{
	phi_attr_t *attr;
	(void) arch_env;

	assert(mode_is_datab(get_irn_mode(node)));

	attr = get_Phi_attr(node);
	memcpy(&attr->req, req, sizeof(req[0]));
}

void be_set_phi_flags(const arch_env_t *arch_env, ir_node *node,
                      arch_irn_flags_t flags)
{
	phi_attr_t *attr;
	(void) arch_env;

	assert(mode_is_datab(get_irn_mode(node)));

	attr = get_Phi_attr(node);
	attr->flags = flags;
}

static void phi_set_irn_reg(ir_node *irn, const arch_register_t *reg)
{
	phi_attr_t *attr = get_Phi_attr(irn);
	attr->reg = reg;
}

static const arch_register_t *phi_get_irn_reg(const ir_node *irn)
{
	phi_attr_t *attr = get_Phi_attr(irn);
	return attr->reg;
}

static arch_irn_class_t phi_classify(const ir_node *irn)
{
	(void) irn;
	return arch_irn_class_normal;
}

static arch_irn_flags_t phi_get_flags(const ir_node *irn)
{
	phi_attr_t *attr = get_Phi_attr(irn);
	return attr->flags;
}

static ir_entity *phi_get_frame_entity(const ir_node *irn)
{
	(void) irn;
	return NULL;
}

static void phi_set_frame_entity(ir_node *irn, ir_entity *ent)
{
	(void) irn;
	(void) ent;
	assert(0);
}

static void phi_set_frame_offset(ir_node *irn, int bias)
{
	(void) irn;
	(void) bias;
	assert(0);
}

static int phi_get_sp_bias(const ir_node *irn)
{
	(void) irn;
	return 0;
}

static const arch_irn_ops_t phi_irn_ops = {
	phi_get_irn_reg_req,
	phi_set_irn_reg,
	phi_get_irn_reg,
	phi_classify,
	phi_get_flags,
	phi_get_frame_entity,
	phi_set_frame_entity,
	phi_set_frame_offset,
	phi_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

void be_phi_handler_new(be_main_env_t *env)
{
	phi_handler.arch_env  = env->arch_env;
	phi_handler.phi_attrs = pmap_create();
	op_Phi->ops.be_ops    = &phi_irn_ops;
}

void be_phi_handler_free(void)
{
	pmap_destroy(phi_handler.phi_attrs);
	phi_handler.phi_attrs = NULL;
	op_Phi->ops.be_ops    = NULL;
}

void be_phi_handler_reset(void)
{
	if(phi_handler.phi_attrs)
		pmap_destroy(phi_handler.phi_attrs);
	phi_handler.phi_attrs = pmap_create();
}

/*
  _   _           _        ____                        _
 | \ | | ___   __| | ___  |  _ \ _   _ _ __ ___  _ __ (_)_ __   __ _
 |  \| |/ _ \ / _` |/ _ \ | | | | | | | '_ ` _ \| '_ \| | '_ \ / _` |
 | |\  | (_) | (_| |  __/ | |_| | |_| | | | | | | |_) | | | | | (_| |
 |_| \_|\___/ \__,_|\___| |____/ \__,_|_| |_| |_| .__/|_|_| |_|\__, |
                                                |_|            |___/
*/

/**
 * Dumps a register requirement to a file.
 */
static void dump_node_req(FILE *f, int idx, const arch_register_req_t *req,
                          const ir_node *node)
{
	int did_something = 0;
	char buf[16];
	const char *prefix = buf;

	snprintf(buf, sizeof(buf), "#%d ", idx);
	buf[sizeof(buf) - 1] = '\0';

	if(req->cls != 0) {
		char tmp[256];
		fprintf(f, prefix);
		arch_register_req_format(tmp, sizeof(tmp), req, node);
		fprintf(f, "%s", tmp);
		did_something = 1;
	}

	if(did_something)
		fprintf(f, "\n");
}

/**
 * Dumps node register requirements to a file.
 */
static void dump_node_reqs(FILE *f, ir_node *node)
{
	int i;
	be_node_attr_t *a = get_irn_attr(node);
	int len = ARR_LEN(a->reg_data);

	fprintf(f, "registers: \n");
	for(i = 0; i < len; ++i) {
		be_reg_data_t *rd = &a->reg_data[i];
		if(rd->reg)
			fprintf(f, "#%d: %s\n", i, rd->reg->name);
	}

	fprintf(f, "in requirements:\n");
	for(i = 0; i < len; ++i) {
		dump_node_req(f, i, &a->reg_data[i].in_req.req, node);
	}

	fprintf(f, "\nout requirements:\n");
	for(i = 0; i < len; ++i) {
		dump_node_req(f, i, &a->reg_data[i].req.req, node);
	}
}

/**
 * ir_op-Operation: dump a be node to file
 */
static int dump_node(ir_node *irn, FILE *f, dump_reason_t reason)
{
	be_node_attr_t *at = get_irn_attr(irn);

	assert(is_be_node(irn));

	switch(reason) {
		case dump_node_opcode_txt:
			fprintf(f, get_op_name(get_irn_op(irn)));
			break;
		case dump_node_mode_txt:
			if(be_is_Perm(irn) || be_is_Copy(irn) || be_is_CopyKeep(irn)) {
				fprintf(f, " %s", get_mode_name(get_irn_mode(irn)));
			}
			break;
		case dump_node_nodeattr_txt:
			if(be_is_Call(irn)) {
				be_call_attr_t *a = (be_call_attr_t *) at;
				if (a->ent)
					fprintf(f, " [%s] ", get_entity_name(a->ent));
			}
			if(be_is_IncSP(irn)) {
				const be_incsp_attr_t *attr = get_irn_generic_attr_const(irn);
				if(attr->offset == BE_STACK_FRAME_SIZE_EXPAND) {
					fprintf(f, " [Setup Stackframe] ");
				} else if(attr->offset == BE_STACK_FRAME_SIZE_SHRINK) {
					fprintf(f, " [Destroy Stackframe] ");
				} else {
					fprintf(f, " [%d] ", attr->offset);
				}
			}
			break;
		case dump_node_info_txt:
			dump_node_reqs(f, irn);

			if(be_has_frame_entity(irn)) {
				be_frame_attr_t *a = (be_frame_attr_t *) at;
				if (a->ent) {
					unsigned size = get_type_size_bytes(get_entity_type(a->ent));
					ir_fprintf(f, "frame entity: %+F, offset 0x%x (%d), size 0x%x (%d) bytes\n",
					  a->ent, a->offset, a->offset, size, size);
				}

			}

			switch (get_irn_opcode(irn)) {
			case beo_IncSP:
				{
					be_incsp_attr_t *a = (be_incsp_attr_t *) at;
					if (a->offset == BE_STACK_FRAME_SIZE_EXPAND)
						fprintf(f, "offset: FRAME_SIZE\n");
					else if(a->offset == BE_STACK_FRAME_SIZE_SHRINK)
						fprintf(f, "offset: -FRAME SIZE\n");
					else
						fprintf(f, "offset: %u\n", a->offset);
				}
				break;
			case beo_Call:
				{
					be_call_attr_t *a = (be_call_attr_t *) at;

					if (a->ent)
						fprintf(f, "\ncalling: %s\n", get_entity_name(a->ent));
				}
				break;
			case beo_MemPerm:
				{
					int i;
					for(i = 0; i < be_get_MemPerm_entity_arity(irn); ++i) {
						ir_entity *in, *out;
						in = be_get_MemPerm_in_entity(irn, i);
						out = be_get_MemPerm_out_entity(irn, i);
						if(in) {
							fprintf(f, "\nin[%d]: %s\n", i, get_entity_name(in));
						}
						if(out) {
							fprintf(f, "\nout[%d]: %s\n", i, get_entity_name(out));
						}
					}
				}
				break;

			default:
				break;
			}
	}

	return 0;
}

/**
 * ir_op-Operation:
 * Copies the backend specific attributes from old node to new node.
 */
static void copy_attr(const ir_node *old_node, ir_node *new_node)
{
	const be_node_attr_t *old_attr = get_irn_attr_const(old_node);
	be_node_attr_t *new_attr = get_irn_attr(new_node);
	struct obstack *obst = get_irg_obstack(get_irn_irg(new_node));
	unsigned i, len;

	assert(is_be_node(old_node));
	assert(is_be_node(new_node));

	memcpy(new_attr, old_attr, get_op_attr_size(get_irn_op(old_node)));
	new_attr->reg_data = NULL;

	if(old_attr->reg_data != NULL)
		len = ARR_LEN(old_attr->reg_data);
	else
		len = 0;

	if(get_irn_op(old_node)->opar == oparity_dynamic
			|| be_is_RegParams(old_node)) {
		new_attr->reg_data = NEW_ARR_F(be_reg_data_t, len);
	} else {
		new_attr->reg_data = NEW_ARR_D(be_reg_data_t, obst, len);
	}

	if(len > 0) {
		memcpy(new_attr->reg_data, old_attr->reg_data, len * sizeof(be_reg_data_t));
		for(i = 0; i < len; ++i) {
			const be_reg_data_t *rd = &old_attr->reg_data[i];
			be_reg_data_t *newrd = &new_attr->reg_data[i];
			if(arch_register_req_is(&rd->req.req, limited)) {
				const arch_register_req_t *req = &rd->req.req;
				arch_register_req_t *new_req = &newrd->req.req;
				new_req->limited
					= rbitset_duplicate_obstack_alloc(obst, req->limited, req->cls->n_regs);
			}
			if(arch_register_req_is(&rd->in_req.req, limited)) {
				const arch_register_req_t *req = &rd->in_req.req;
				arch_register_req_t *new_req = &newrd->in_req.req;
				new_req->limited
					= rbitset_duplicate_obstack_alloc(obst, req->limited, req->cls->n_regs);
			}
		}
	}
}

static const ir_op_ops be_node_op_ops = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	copy_attr,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	dump_node,
	NULL,
	&be_node_irn_ops
};

int is_be_node(const ir_node *irn)
{
	return get_op_ops(get_irn_op(irn))->be_ops == &be_node_irn_ops;
}

void be_node_init(void) {
	static int inited = 0;

	if(inited)
		return;

	inited = 1;

	/* Acquire all needed opcodes. */
	op_be_Spill      = new_ir_op(beo_Spill,     "be_Spill",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_frame_attr_t),   &be_node_op_ops);
	op_be_Reload     = new_ir_op(beo_Reload,    "be_Reload",    op_pin_state_pinned, N,   oparity_zero,     0, sizeof(be_frame_attr_t),   &be_node_op_ops);
	op_be_Perm       = new_ir_op(beo_Perm,      "be_Perm",      op_pin_state_pinned, N,   oparity_variable, 0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_MemPerm    = new_ir_op(beo_MemPerm,   "be_MemPerm",   op_pin_state_pinned, N,   oparity_variable, 0, sizeof(be_memperm_attr_t), &be_node_op_ops);
	op_be_Copy       = new_ir_op(beo_Copy,      "be_Copy",      op_pin_state_floats, N,   oparity_unary,    0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_Keep       = new_ir_op(beo_Keep,      "be_Keep",      op_pin_state_pinned, K,   oparity_dynamic,  0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_CopyKeep   = new_ir_op(beo_CopyKeep,  "be_CopyKeep",  op_pin_state_pinned, K,   oparity_variable, 0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_Call       = new_ir_op(beo_Call,      "be_Call",      op_pin_state_pinned, F|M, oparity_variable, 0, sizeof(be_call_attr_t),    &be_node_op_ops);
	op_be_Return     = new_ir_op(beo_Return,    "be_Return",    op_pin_state_pinned, X,   oparity_dynamic,  0, sizeof(be_return_attr_t),  &be_node_op_ops);
	op_be_AddSP      = new_ir_op(beo_AddSP,     "be_AddSP",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_SubSP      = new_ir_op(beo_SubSP,     "be_SubSP",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_IncSP      = new_ir_op(beo_IncSP,     "be_IncSP",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_incsp_attr_t),   &be_node_op_ops);
	op_be_RegParams  = new_ir_op(beo_RegParams, "be_RegParams", op_pin_state_pinned, N,   oparity_zero,     0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_FrameAddr  = new_ir_op(beo_FrameAddr, "be_FrameAddr", op_pin_state_floats, N,   oparity_unary,    0, sizeof(be_frame_attr_t),   &be_node_op_ops);
	op_be_Barrier    = new_ir_op(beo_Barrier,   "be_Barrier",   op_pin_state_pinned, N,   oparity_dynamic,  0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_Unwind     = new_ir_op(beo_Unwind,    "be_Unwind",    op_pin_state_pinned, X,   oparity_zero,     0, sizeof(be_node_attr_t),    &be_node_op_ops);

	op_be_Spill->ops.node_cmp_attr     = FrameAddr_cmp_attr;
	op_be_Reload->ops.node_cmp_attr    = FrameAddr_cmp_attr;
	op_be_Perm->ops.node_cmp_attr      = node_cmp_attr;
	op_be_MemPerm->ops.node_cmp_attr   = node_cmp_attr;
	op_be_Copy->ops.node_cmp_attr      = node_cmp_attr;
	op_be_Keep->ops.node_cmp_attr      = node_cmp_attr;
	op_be_CopyKeep->ops.node_cmp_attr  = node_cmp_attr;
	op_be_Call->ops.node_cmp_attr      = Call_cmp_attr;
	op_be_Return->ops.node_cmp_attr    = Return_cmp_attr;
	op_be_AddSP->ops.node_cmp_attr     = node_cmp_attr;
	op_be_SubSP->ops.node_cmp_attr     = node_cmp_attr;
	op_be_IncSP->ops.node_cmp_attr     = IncSP_cmp_attr;
	op_be_RegParams->ops.node_cmp_attr = node_cmp_attr;
	op_be_FrameAddr->ops.node_cmp_attr = FrameAddr_cmp_attr;
	op_be_Barrier->ops.node_cmp_attr   = node_cmp_attr;
	op_be_Unwind->ops.node_cmp_attr    = node_cmp_attr;
}
