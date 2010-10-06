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
 * @brief       This file implements the common parts of IR transformation from
 *              firm into ia32-Firm.
 * @author      Matthias Braun, Sebastian Buchwald
 * @version     $Id: ia32_common_transform.c 21012 2008-08-06 13:35:17Z beck $
 */
#include "config.h"

#include "error.h"
#include "ircons.h"
#include "irprintf.h"
#include "typerep.h"
#include "bitset.h"
#include "heights.h"

#include "../betranshlp.h"
#include "../beirg.h"
#include "../beabi.h"

#include "ia32_architecture.h"
#include "ia32_common_transform.h"
#include "ia32_new_nodes.h"

#include "gen_ia32_new_nodes.h"
#include "gen_ia32_regalloc_if.h"

ir_heights_t *heights = NULL;

static int check_immediate_constraint(long val, char immediate_constraint_type)
{
	switch (immediate_constraint_type) {
		case 0:
		case 'i': return 1;

		case 'I': return    0 <= val && val <=  31;
		case 'J': return    0 <= val && val <=  63;
		case 'K': return -128 <= val && val <= 127;
		case 'L': return val == 0xff || val == 0xffff;
		case 'M': return    0 <= val && val <=   3;
		case 'N': return    0 <= val && val <= 255;
		case 'O': return    0 <= val && val <= 127;

		default: panic("Invalid immediate constraint found");
	}
}

/**
 * Get a primitive type for a mode with alignment 16.
 */
static ir_type *ia32_get_prim_type(pmap *types, ir_mode *mode)
{
	ir_type *res = pmap_get(types, mode);
	if (res != NULL)
		return res;

	res = new_type_primitive(mode);
	if (get_mode_size_bits(mode) >= 80) {
		set_type_alignment_bytes(res, 16);
	}
	pmap_insert(types, mode, res);
	return res;
}

ir_entity *create_float_const_entity(ir_node *cnst)
{
	ir_graph         *irg      = get_irn_irg(cnst);
	const arch_env_t *arch_env = be_get_irg_arch_env(irg);
	ia32_isa_t       *isa      = (ia32_isa_t*) arch_env;
	tarval           *tv       = get_Const_tarval(cnst);
	ir_entity        *res      = pmap_get(isa->tv_ent, tv);
	ir_initializer_t *initializer;
	ir_mode          *mode;
	ir_type          *tp;

	if (res != NULL)
		return res;

	mode = get_tarval_mode(tv);

	if (! ia32_cg_config.use_sse2) {
		/* try to reduce the mode to produce smaller sized entities */
		if (mode != mode_F) {
			if (tarval_ieee754_can_conv_lossless(tv, mode_F)) {
				mode = mode_F;
				tv = tarval_convert_to(tv, mode);
			} else if (mode != mode_D) {
				if (tarval_ieee754_can_conv_lossless(tv, mode_D)) {
					mode = mode_D;
					tv = tarval_convert_to(tv, mode);
				}
			}
		}
	}

	tp  = ia32_get_prim_type(isa->types, mode);
	res = new_entity(get_glob_type(), id_unique("C%u"), tp);
	set_entity_ld_ident(res, get_entity_ident(res));
	set_entity_visibility(res, ir_visibility_private);
	add_entity_linkage(res, IR_LINKAGE_CONSTANT);

	initializer = create_initializer_tarval(tv);
	set_entity_initializer(res, initializer);

	pmap_insert(isa->tv_ent, tv, res);
	return res;
}

ir_node *ia32_create_Immediate(ir_entity *symconst, int symconst_sign, long val)
{
	ir_graph *irg         = current_ir_graph;
	ir_node  *start_block = get_irg_start_block(irg);
	ir_node  *immediate   = new_bd_ia32_Immediate(NULL, start_block, symconst,
			symconst_sign, no_pic_adjust, val);
	arch_set_irn_register(immediate, &ia32_registers[REG_GP_NOREG]);

	return immediate;
}

const arch_register_t *ia32_get_clobber_register(const char *clobber)
{
	const arch_register_t       *reg = NULL;
	int                          c;
	size_t                       r;
	const arch_register_class_t *cls;

	/* TODO: construct a hashmap instead of doing linear search for clobber
	 * register */
	for (c = 0; c < N_IA32_CLASSES; ++c) {
		cls = & ia32_reg_classes[c];
		for (r = 0; r < cls->n_regs; ++r) {
			const arch_register_t *temp_reg = arch_register_for_index(cls, r);
			if (strcmp(temp_reg->name, clobber) == 0
					|| (c == CLASS_ia32_gp && strcmp(temp_reg->name+1, clobber) == 0)) {
				reg = temp_reg;
				break;
			}
		}
		if (reg != NULL)
			break;
	}

	return reg;
}

int ia32_mode_needs_gp_reg(ir_mode *mode)
{
	if (mode == mode_fpcw)
		return 0;
	if (get_mode_size_bits(mode) > 32)
		return 0;
	return mode_is_int(mode) || mode_is_reference(mode) || mode == mode_b;
}

static void parse_asm_constraints(constraint_t *constraint, const char *c,
                           int is_output)
{
	char                         immediate_type     = '\0';
	unsigned                     limited            = 0;
	const arch_register_class_t *cls                = NULL;
	int                          memory_possible       = 0;
	int                          all_registers_allowed = 0;
	int                          p;
	int                          same_as = -1;

	memset(constraint, 0, sizeof(constraint[0]));
	constraint->same_as = -1;

	if (*c == 0) {
		/* a memory constraint: no need to do anything in backend about it
		 * (the dependencies are already respected by the memory edge of
		 * the node) */
		return;
	}

	/* TODO: improve error messages with node and source info. (As users can
	 * easily hit these) */
	while (*c != 0) {
		switch (*c) {
		case ' ':
		case '\t':
		case '\n':
			break;

		/* Skip out/in-out marker */
		case '=': break;
		case '+': break;

		case '&': break;

		case '*':
			++c;
			break;
		case '#':
			while (*c != 0 && *c != ',')
				++c;
			break;

		case 'a':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EAX;
			break;
		case 'b':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EBX;
			break;
		case 'c':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_ECX;
			break;
		case 'd':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EDX;
			break;
		case 'D':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EDI;
			break;
		case 'S':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_ESI;
			break;
		case 'Q':
		case 'q':
			/* q means lower part of the regs only, this makes no
			 * difference to Q for us (we only assign whole registers) */
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EAX | 1 << REG_GP_EBX | 1 << REG_GP_ECX |
			           1 << REG_GP_EDX;
			break;
		case 'A':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EAX | 1 << REG_GP_EDX;
			break;
		case 'l':
			assert(cls == NULL || cls == &ia32_reg_classes[CLASS_ia32_gp]);
			cls      = &ia32_reg_classes[CLASS_ia32_gp];
			limited |= 1 << REG_GP_EAX | 1 << REG_GP_EBX | 1 << REG_GP_ECX |
			           1 << REG_GP_EDX | 1 << REG_GP_ESI | 1 << REG_GP_EDI |
			           1 << REG_GP_EBP;
			break;

		case 'R':
		case 'r':
		case 'p':
			if (cls != NULL && cls != &ia32_reg_classes[CLASS_ia32_gp])
				panic("multiple register classes not supported");
			cls                   = &ia32_reg_classes[CLASS_ia32_gp];
			all_registers_allowed = 1;
			break;

		case 'f':
		case 't':
		case 'u':
			/* TODO: mark values so the x87 simulator knows about t and u */
			if (cls != NULL && cls != &ia32_reg_classes[CLASS_ia32_vfp])
				panic("multiple register classes not supported");
			cls                   = &ia32_reg_classes[CLASS_ia32_vfp];
			all_registers_allowed = 1;
			break;

		case 'Y':
		case 'x':
			if (cls != NULL && cls != &ia32_reg_classes[CLASS_ia32_xmm])
				panic("multiple register classes not supproted");
			cls                   = &ia32_reg_classes[CLASS_ia32_xmm];
			all_registers_allowed = 1;
			break;

		case 'I':
		case 'J':
		case 'K':
		case 'L':
		case 'M':
		case 'N':
		case 'O':
			if (cls != NULL && cls != &ia32_reg_classes[CLASS_ia32_gp])
				panic("multiple register classes not supported");
			if (immediate_type != '\0')
				panic("multiple immediate types not supported");
			cls            = &ia32_reg_classes[CLASS_ia32_gp];
			immediate_type = *c;
			break;
		case 'n':
		case 'i':
			if (cls != NULL && cls != &ia32_reg_classes[CLASS_ia32_gp])
				panic("multiple register classes not supported");
			if (immediate_type != '\0')
				panic("multiple immediate types not supported");
			cls            = &ia32_reg_classes[CLASS_ia32_gp];
			immediate_type = 'i';
			break;

		case 'X':
		case 'g':
			if (cls != NULL && cls != &ia32_reg_classes[CLASS_ia32_gp])
				panic("multiple register classes not supported");
			if (immediate_type != '\0')
				panic("multiple immediate types not supported");
			immediate_type        = 'i';
			cls                   = &ia32_reg_classes[CLASS_ia32_gp];
			all_registers_allowed = 1;
			memory_possible       = 1;
			break;

		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			if (is_output)
				panic("can only specify same constraint on input");

			sscanf(c, "%d%n", &same_as, &p);
			if (same_as >= 0) {
				c += p;
				continue;
			}
			break;

		case 'm':
		case 'o':
		case 'V':
			/* memory constraint no need to do anything in backend about it
			 * (the dependencies are already respected by the memory edge of
			 * the node) */
			memory_possible = 1;
			break;

		case 'E': /* no float consts yet */
		case 'F': /* no float consts yet */
		case 's': /* makes no sense on x86 */
		case '<': /* no autodecrement on x86 */
		case '>': /* no autoincrement on x86 */
		case 'C': /* sse constant not supported yet */
		case 'G': /* 80387 constant not supported yet */
		case 'y': /* we don't support mmx registers yet */
		case 'Z': /* not available in 32 bit mode */
		case 'e': /* not available in 32 bit mode */
			panic("unsupported asm constraint '%c' found in (%+F)",
			      *c, current_ir_graph);
			break;
		default:
			panic("unknown asm constraint '%c' found in (%+F)", *c,
			      current_ir_graph);
			break;
		}
		++c;
	}

	if (same_as >= 0) {
		if (cls != NULL)
			panic("same as and register constraint not supported");
		if (immediate_type != '\0')
			panic("same as and immediate constraint not supported");
	}

	if (cls == NULL && same_as < 0) {
		if (!memory_possible)
			panic("no constraint specified for assembler input");
	}

	constraint->same_as               = same_as;
	constraint->cls                   = cls;
	constraint->allowed_registers     = limited;
	constraint->all_registers_allowed = all_registers_allowed;
	constraint->memory_possible       = memory_possible;
	constraint->immediate_type        = immediate_type;
}

static bool can_match(const arch_register_req_t *in,
                      const arch_register_req_t *out)
{
	if (in->cls != out->cls)
		return false;
	if ( (in->type & arch_register_req_type_limited) == 0
		|| (out->type & arch_register_req_type_limited) == 0 )
		return true;

	return (*in->limited & *out->limited) != 0;
}

static inline ir_node *get_new_node(ir_node *node)
{
#ifdef FIRM_GRGEN_BE
	if (be_transformer == TRANSFORMER_DEFAULT) {
		return be_transform_node(node);
	} else {
		return node;
	}
#else
	return be_transform_node(node);
#endif
}

ir_node *gen_ASM(ir_node *node)
{
	ir_node                    *block     = get_nodes_block(node);
	ir_node                    *new_block = get_new_node(block);
	dbg_info                   *dbgi      = get_irn_dbg_info(node);
	int                         i, arity;
	int                         value_arity;
	int                         out_idx;
	ir_node                   **in;
	ir_node                    *new_node;
	int                         out_arity;
	int                         n_out_constraints;
	int                         n_clobbers;
	const arch_register_req_t **out_reg_reqs;
	const arch_register_req_t **in_reg_reqs;
	ia32_asm_reg_t             *register_map;
	unsigned                    reg_map_size = 0;
	struct obstack             *obst;
	const ir_asm_constraint    *in_constraints;
	const ir_asm_constraint    *out_constraints;
	ident                     **clobbers;
	int                         clobbers_flags = 0;
	unsigned                    clobber_bits[N_IA32_CLASSES];
	int                         out_size;
	backend_info_t             *info;

	memset(&clobber_bits, 0, sizeof(clobber_bits));

	/* workaround for lots of buggy code out there as most people think volatile
	 * asm is enough for everything and forget the flags (linux kernel, etc.)
	 */
	if (get_irn_pinned(node) == op_pin_state_pinned) {
		clobbers_flags = 1;
	}

	arity = get_irn_arity(node);
	in    = ALLOCANZ(ir_node*, arity);

	clobbers   = get_ASM_clobbers(node);
	n_clobbers = 0;
	for (i = 0; i < get_ASM_n_clobbers(node); ++i) {
		const arch_register_req_t *req;
		const char                *c = get_id_str(clobbers[i]);

		if (strcmp(c, "memory") == 0)
			continue;
		if (strcmp(c, "cc") == 0) {
			clobbers_flags = 1;
			continue;
		}

		req = parse_clobber(c);
		clobber_bits[req->cls->index] |= *req->limited;

		n_clobbers++;
	}
	n_out_constraints = get_ASM_n_output_constraints(node);
	out_arity         = n_out_constraints + n_clobbers;

	in_constraints  = get_ASM_input_constraints(node);
	out_constraints = get_ASM_output_constraints(node);

	/* determine size of register_map */
	for (out_idx = 0; out_idx < n_out_constraints; ++out_idx) {
		const ir_asm_constraint *constraint = &out_constraints[out_idx];
		if (constraint->pos > reg_map_size)
			reg_map_size = constraint->pos;
	}
	for (i = 0; i < arity; ++i) {
		const ir_asm_constraint *constraint = &in_constraints[i];
		if (constraint->pos > reg_map_size)
			reg_map_size = constraint->pos;
	}
	++reg_map_size;

	obst         = get_irg_obstack(current_ir_graph);
	register_map = NEW_ARR_D(ia32_asm_reg_t, obst, reg_map_size);
	memset(register_map, 0, reg_map_size * sizeof(register_map[0]));

	/* construct output constraints */
	out_size = out_arity + 1;
	out_reg_reqs = obstack_alloc(obst, out_size * sizeof(out_reg_reqs[0]));

	for (out_idx = 0; out_idx < n_out_constraints; ++out_idx) {
		const ir_asm_constraint   *constraint = &out_constraints[out_idx];
		const char                *c       = get_id_str(constraint->constraint);
		unsigned                   pos        = constraint->pos;
		constraint_t               parsed_constraint;
		const arch_register_req_t *req;

		parse_asm_constraints(&parsed_constraint, c, 1);
		req = make_register_req(&parsed_constraint, n_out_constraints,
		                        out_reg_reqs, out_idx);
		out_reg_reqs[out_idx] = req;

		register_map[pos].use_input = 0;
		register_map[pos].valid     = 1;
		register_map[pos].memory    = 0;
		register_map[pos].inout_pos = out_idx;
		register_map[pos].mode      = constraint->mode;
	}

	/* inputs + input constraints */
	in_reg_reqs = obstack_alloc(obst, arity * sizeof(in_reg_reqs[0]));
	for (i = 0; i < arity; ++i) {
		ir_node                   *pred         = get_irn_n(node, i);
		const ir_asm_constraint   *constraint   = &in_constraints[i];
		ident                     *constr_id    = constraint->constraint;
		const char                *c            = get_id_str(constr_id);
		unsigned                   pos          = constraint->pos;
		int                        is_memory_op = 0;
		ir_node                   *input        = NULL;
		unsigned                   r_clobber_bits;
		constraint_t               parsed_constraint;
		const arch_register_req_t *req;

		parse_asm_constraints(&parsed_constraint, c, 0);
		if (parsed_constraint.cls != NULL) {
			r_clobber_bits = clobber_bits[parsed_constraint.cls->index];
			if (r_clobber_bits != 0) {
				if (parsed_constraint.all_registers_allowed) {
					parsed_constraint.all_registers_allowed = 0;
					be_abi_set_non_ignore_regs(be_get_irg_abi(current_ir_graph),
							parsed_constraint.cls,
							&parsed_constraint.allowed_registers);
				}
				parsed_constraint.allowed_registers &= ~r_clobber_bits;
			}
		}

		req = make_register_req(&parsed_constraint, n_out_constraints,
		                        out_reg_reqs, i);
		in_reg_reqs[i] = req;

		if (parsed_constraint.immediate_type != '\0') {
			char imm_type = parsed_constraint.immediate_type;
			input = try_create_Immediate(pred, imm_type);
		}

		if (input == NULL) {
			ir_node *pred = get_irn_n(node, i);
			input = get_new_node(pred);

			if (parsed_constraint.cls == NULL
					&& parsed_constraint.same_as < 0) {
				is_memory_op = 1;
			} else if (parsed_constraint.memory_possible) {
				/* TODO: match Load or Load/Store if memory possible is set */
			}
		}
		in[i] = input;

		register_map[pos].use_input = 1;
		register_map[pos].valid     = 1;
		register_map[pos].memory    = is_memory_op;
		register_map[pos].inout_pos = i;
		register_map[pos].mode      = constraint->mode;
	}

	/* parse clobbers */
	for (i = 0; i < get_ASM_n_clobbers(node); ++i) {
		const char                *c = get_id_str(clobbers[i]);
		const arch_register_req_t *req;

		if (strcmp(c, "memory") == 0 || strcmp(c, "cc") == 0)
			continue;

		req = parse_clobber(c);
		out_reg_reqs[out_idx] = req;
		++out_idx;
	}

	/* count inputs which are real values (and not memory) */
	value_arity = 0;
	for (i = 0; i < arity; ++i) {
		ir_node *in = get_irn_n(node, i);
		if (get_irn_mode(in) == mode_M)
			continue;
		++value_arity;
	}

	/* Attempt to make ASM node register pressure faithful.
	 * (This does not work for complicated cases yet!)
	 *
	 * Algorithm: Check if there are fewer inputs or outputs (I will call this
	 * the smaller list). Then try to match each constraint of the smaller list
	 * to 1 of the other list. If we can't match it, then we have to add a dummy
	 * input/output to the other list
	 *
	 * FIXME: This is still broken in lots of cases. But at least better than
	 *        before...
	 * FIXME: need to do this per register class...
	 */
	if (out_arity <= value_arity) {
		int       orig_arity = arity;
		int       in_size    = arity;
		int       o;
		bitset_t *used_ins = bitset_alloca(arity);
		for (o = 0; o < out_arity; ++o) {
			int   i;
			const arch_register_req_t *outreq = out_reg_reqs[o];

			if (outreq->cls == NULL) {
				continue;
			}

			for (i = 0; i < orig_arity; ++i) {
				const arch_register_req_t *inreq;
				if (bitset_is_set(used_ins, i))
					continue;
				inreq = in_reg_reqs[i];
				if (!can_match(outreq, inreq))
					continue;
				bitset_set(used_ins, i);
				break;
			}
			/* did we find any match? */
			if (i < orig_arity)
				continue;

			/* we might need more space in the input arrays */
			if (arity >= in_size) {
				const arch_register_req_t **new_in_reg_reqs;
				ir_node             **new_in;

				in_size *= 2;
				new_in_reg_reqs
					= obstack_alloc(obst, in_size*sizeof(in_reg_reqs[0]));
				memcpy(new_in_reg_reqs, in_reg_reqs, arity * sizeof(new_in_reg_reqs[0]));
				new_in = ALLOCANZ(ir_node*, in_size);
				memcpy(new_in, in, arity*sizeof(new_in[0]));

				in_reg_reqs = new_in_reg_reqs;
				in          = new_in;
			}

			/* add a new (dummy) input which occupies the register */
			assert(outreq->type & arch_register_req_type_limited);
			in_reg_reqs[arity] = outreq;
			in[arity]          = new_bd_ia32_ProduceVal(NULL, block);
			be_dep_on_frame(in[arity]);
			++arity;
		}
	} else {
		int       i;
		bitset_t *used_outs = bitset_alloca(out_arity);
		int       orig_out_arity = out_arity;
		for (i = 0; i < arity; ++i) {
			int   o;
			const arch_register_req_t *inreq = in_reg_reqs[i];

			if (inreq->cls == NULL) {
				continue;
			}

			for (o = 0; o < orig_out_arity; ++o) {
				const arch_register_req_t *outreq;
				if (bitset_is_set(used_outs, o))
					continue;
				outreq = out_reg_reqs[o];
				if (!can_match(outreq, inreq))
					continue;
				bitset_set(used_outs, i);
				break;
			}
			/* did we find any match? */
			if (o < orig_out_arity)
				continue;

			/* we might need more space in the output arrays */
			if (out_arity >= out_size) {
				const arch_register_req_t **new_out_reg_reqs;

				out_size *= 2;
				new_out_reg_reqs
					= obstack_alloc(obst, out_size*sizeof(out_reg_reqs[0]));
				memcpy(new_out_reg_reqs, out_reg_reqs,
				       out_arity * sizeof(new_out_reg_reqs[0]));
				out_reg_reqs = new_out_reg_reqs;
			}

			/* add a new (dummy) output which occupies the register */
			assert(inreq->type & arch_register_req_type_limited);
			out_reg_reqs[out_arity] = inreq;
			++out_arity;
		}
	}

	/* append none register requirement for the memory output */
	if (out_arity + 1 >= out_size) {
		const arch_register_req_t **new_out_reg_reqs;

		out_size = out_arity + 1;
		new_out_reg_reqs
			= obstack_alloc(obst, out_size*sizeof(out_reg_reqs[0]));
		memcpy(new_out_reg_reqs, out_reg_reqs,
			   out_arity * sizeof(new_out_reg_reqs[0]));
		out_reg_reqs = new_out_reg_reqs;
	}

	/* add a new (dummy) output which occupies the register */
	out_reg_reqs[out_arity] = arch_no_register_req;
	++out_arity;

	new_node = new_bd_ia32_Asm(dbgi, new_block, arity, in, out_arity,
	                           get_ASM_text(node), register_map);

	if (arity == 0)
		be_dep_on_frame(new_node);

	info = be_get_info(new_node);
	for (i = 0; i < out_arity; ++i) {
		info->out_infos[i].req = out_reg_reqs[i];
	}
	arch_set_in_register_reqs(new_node, in_reg_reqs);

	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

ir_node *gen_CopyB(ir_node *node)
{
	ir_node  *block    = get_new_node(get_nodes_block(node));
	ir_node  *src      = get_CopyB_src(node);
	ir_node  *new_src  = get_new_node(src);
	ir_node  *dst      = get_CopyB_dst(node);
	ir_node  *new_dst  = get_new_node(dst);
	ir_node  *mem      = get_CopyB_mem(node);
	ir_node  *new_mem  = get_new_node(mem);
	ir_node  *res      = NULL;
	dbg_info *dbgi     = get_irn_dbg_info(node);
	int      size      = get_type_size_bytes(get_CopyB_type(node));
	int      rem;

	/* If we have to copy more than 32 bytes, we use REP MOVSx and */
	/* then we need the size explicitly in ECX.                    */
	if (size >= 32 * 4) {
		rem = size & 0x3; /* size % 4 */
		size >>= 2;

		res = new_bd_ia32_Const(dbgi, block, NULL, 0, 0, size);
		be_dep_on_frame(res);

		res = new_bd_ia32_CopyB(dbgi, block, new_dst, new_src, res, new_mem, rem);
	} else {
		if (size == 0) {
			ir_fprintf(stderr, "Optimization warning copyb %+F with size <4\n",
			           node);
		}
		res = new_bd_ia32_CopyB_i(dbgi, block, new_dst, new_src, new_mem, size);
	}

	SET_IA32_ORIG_NODE(res, node);

	return res;
}

ir_node *gen_Proj_tls(ir_node *node)
{
	ir_node *block = get_new_node(get_nodes_block(node));
	ir_node *res   = NULL;

	res = new_bd_ia32_LdTls(NULL, block, mode_Iu);

	return res;
}

ir_node *gen_Unknown(ir_node *node)
{
	ir_mode  *mode  = get_irn_mode(node);
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node  *block = get_irg_start_block(irg);
	ir_node  *res   = NULL;

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2) {
			res = new_bd_ia32_xUnknown(dbgi, block);
		} else {
			res = new_bd_ia32_vfldz(dbgi, block);
		}
	} else if (ia32_mode_needs_gp_reg(mode)) {
		res = new_bd_ia32_Unknown(dbgi, block);
	} else {
		panic("unsupported Unknown-Mode");
	}

	be_dep_on_frame(res);
	return res;
}

const arch_register_req_t *make_register_req(const constraint_t *constraint,
		int n_outs, const arch_register_req_t **out_reqs, int pos)
{
	struct obstack      *obst    = get_irg_obstack(current_ir_graph);
	int                  same_as = constraint->same_as;
	arch_register_req_t *req;

	if (same_as >= 0) {
		const arch_register_req_t *other_constr;

		if (same_as >= n_outs)
			panic("invalid output number in same_as constraint");

		other_constr     = out_reqs[same_as];

		req              = obstack_alloc(obst, sizeof(req[0]));
		*req             = *other_constr;
		req->type       |= arch_register_req_type_should_be_same;
		req->other_same  = 1U << pos;
		req->width       = 1;

		/* switch constraints. This is because in firm we have same_as
		 * constraints on the output constraints while in the gcc asm syntax
		 * they are specified on the input constraints */
		out_reqs[same_as] = req;
		return other_constr;
	}

	/* pure memory ops */
	if (constraint->cls == NULL) {
		return arch_no_register_req;
	}

	if (constraint->allowed_registers != 0
			&& !constraint->all_registers_allowed) {
		unsigned *limited_ptr;

		req         = obstack_alloc(obst, sizeof(req[0]) + sizeof(unsigned));
		memset(req, 0, sizeof(req[0]));
		limited_ptr = (unsigned*) (req+1);

		req->type    = arch_register_req_type_limited;
		*limited_ptr = constraint->allowed_registers;
		req->limited = limited_ptr;
	} else {
		req       = obstack_alloc(obst, sizeof(req[0]));
		memset(req, 0, sizeof(req[0]));
		req->type = arch_register_req_type_normal;
	}
	req->cls   = constraint->cls;
	req->width = 1;

	return req;
}

const arch_register_req_t *parse_clobber(const char *clobber)
{
	struct obstack        *obst = get_irg_obstack(current_ir_graph);
	const arch_register_t *reg  = ia32_get_clobber_register(clobber);
	arch_register_req_t   *req;
	unsigned              *limited;

	if (reg == NULL) {
		panic("Register '%s' mentioned in asm clobber is unknown", clobber);
	}

	assert(reg->index < 32);

	limited  = obstack_alloc(obst, sizeof(limited[0]));
	*limited = 1 << reg->index;

	req          = obstack_alloc(obst, sizeof(req[0]));
	memset(req, 0, sizeof(req[0]));
	req->type    = arch_register_req_type_limited;
	req->cls     = arch_register_get_class(reg);
	req->limited = limited;
	req->width   = 1;

	return req;
}


int prevents_AM(ir_node *const block, ir_node *const am_candidate,
                       ir_node *const other)
{
	if (get_nodes_block(other) != block)
		return 0;

	if (is_Sync(other)) {
		int i;

		for (i = get_Sync_n_preds(other) - 1; i >= 0; --i) {
			ir_node *const pred = get_Sync_pred(other, i);

			if (get_nodes_block(pred) != block)
				continue;

			/* Do not block ourselves from getting eaten */
			if (is_Proj(pred) && get_Proj_pred(pred) == am_candidate)
				continue;

			if (!heights_reachable_in_block(heights, pred, am_candidate))
				continue;

			return 1;
		}

		return 0;
	} else {
		/* Do not block ourselves from getting eaten */
		if (is_Proj(other) && get_Proj_pred(other) == am_candidate)
			return 0;

		if (!heights_reachable_in_block(heights, other, am_candidate))
			return 0;

		return 1;
	}
}

ir_node *try_create_Immediate(ir_node *node, char immediate_constraint_type)
{
	long         val = 0;
	ir_entity   *symconst_ent  = NULL;
	ir_mode     *mode;
	ir_node     *cnst          = NULL;
	ir_node     *symconst      = NULL;
	ir_node     *new_node;

	mode = get_irn_mode(node);
	if (!mode_is_int(mode) && !mode_is_reference(mode)) {
		return NULL;
	}

	if (is_Const(node)) {
		cnst     = node;
		symconst = NULL;
	} else if (is_Global(node)) {
		cnst     = NULL;
		symconst = node;
	} else if (is_Add(node)) {
		ir_node *left  = get_Add_left(node);
		ir_node *right = get_Add_right(node);
		if (is_Const(left) && is_Global(right)) {
			cnst     = left;
			symconst = right;
		} else if (is_Global(left) && is_Const(right)) {
			cnst     = right;
			symconst = left;
		}
	} else {
		return NULL;
	}

	if (cnst != NULL) {
		tarval *offset = get_Const_tarval(cnst);
		if (!tarval_is_long(offset)) {
			ir_fprintf(stderr, "Optimisation Warning: tarval of %+F is not a long?\n", cnst);
			return NULL;
		}

		val = get_tarval_long(offset);
		if (!check_immediate_constraint(val, immediate_constraint_type))
			return NULL;
	}
	if (symconst != NULL) {
		if (immediate_constraint_type != 0) {
			/* we need full 32bits for symconsts */
			return NULL;
		}

		symconst_ent = get_Global_entity(symconst);
	}
	if (cnst == NULL && symconst == NULL)
		return NULL;

	new_node = ia32_create_Immediate(symconst_ent, 0, val);
	return new_node;
}
