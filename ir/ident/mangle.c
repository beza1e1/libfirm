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
 * @brief   Methods to manipulate names.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Michael Beck
 * @version $Id$
 */
#include "config.h"

#include <stdio.h>

#include "ident_t.h"
#include "obst.h"

/* Make types visible to allow most efficient access */
#include "entity_t.h"
#include "type_t.h"
#include "tpop_t.h"

/** An obstack used for temporary space */
static struct obstack mangle_obst;

/** returned a mangled type name, currently no mangling */
static inline ident *mangle_type(const ir_type *tp)
{
	assert(tp->kind == k_type);
	return tp->name;
}

ident *id_mangle_entity(const ir_entity *ent)
{
	ident *type_id;
	char *cp;
	int len;
	ident *res;

	type_id = mangle_type(ent->owner);
	obstack_grow(&mangle_obst, get_id_str(type_id), get_id_strlen(type_id));
	obstack_1grow(&mangle_obst,'_');
	obstack_grow(&mangle_obst,get_id_str(ent->name),get_id_strlen(ent->name));
	len = obstack_object_size (&mangle_obst);
	cp = obstack_finish (&mangle_obst);
	res = new_id_from_chars(cp, len);
	obstack_free (&mangle_obst, cp);
	return res;
}


/* Returns a new ident that represents 'firstscnd'. */
ident *id_mangle(const ident *first, const ident *scnd)
{
	char *cp;
	int len;
	ident *res;

	obstack_grow(&mangle_obst, get_id_str(first), get_id_strlen(first));
	obstack_grow(&mangle_obst, get_id_str(scnd), get_id_strlen(scnd));
	len = obstack_object_size (&mangle_obst);
	cp = obstack_finish (&mangle_obst);
	res = new_id_from_chars (cp, len);
	obstack_free (&mangle_obst, cp);
	return res;
}

/** Returns a new ident that represents 'prefixscndsuffix'. */
ident *id_mangle3(const char *prefix, const ident *scnd, const char *suffix)
{
	char *cp;
	int len;
	ident *res;

	obstack_grow(&mangle_obst, prefix, strlen(prefix));
	obstack_grow(&mangle_obst, get_id_str(scnd), get_id_strlen(scnd));
	obstack_grow(&mangle_obst, suffix, strlen(suffix));
	len = obstack_object_size (&mangle_obst);
	cp  = obstack_finish (&mangle_obst);
	res = new_id_from_chars (cp, len);
	obstack_free (&mangle_obst, cp);
	return res;
}

/** Returns a new ident that represents first<c>scnd. */
static ident *id_mangle_3(const ident *first, char c, const ident* scnd)
{
	char *cp;
	int len;
	ident *res;

	obstack_grow(&mangle_obst, get_id_str(first), get_id_strlen(first));
	obstack_1grow(&mangle_obst, c);
	obstack_grow(&mangle_obst,get_id_str(scnd),get_id_strlen(scnd));
	len = obstack_object_size (&mangle_obst);
	cp = obstack_finish (&mangle_obst);
	res = new_id_from_chars (cp, len);
	obstack_free (&mangle_obst, cp);
	return res;
}

/* Returns a new ident that represents first_scnd. */
ident *id_mangle_u(const ident *first, const ident* scnd)
{
	return id_mangle_3(first, '_', scnd);
}

/* Returns a new ident that represents first.scnd. */
ident *id_mangle_dot(const ident *first, const ident* scnd)
{
	return id_mangle_3(first, '.', scnd);
}

/* returns a mangled name for a Win32 function using it's calling convention */
ident *id_decorate_win32_c_fkt(const ir_entity *ent, const ident *id)
{
	ir_type *tp      = get_entity_type(ent);
	unsigned cc_mask = get_method_calling_convention(tp);
	char buf[16];
	int size, i;

	if (IS_CDECL(cc_mask))
		return id_mangle3("_", id, "");
	else if (IS_STDCALL(cc_mask)) {
		size = 0;
		for (i = get_method_n_params(tp) - 1; i >= 0; --i) {
			size += get_type_size_bytes(get_method_param_type(tp, i));
		}

		snprintf(buf, sizeof(buf), "@%d", size);

		if (cc_mask & cc_reg_param)
			return id_mangle3("@", id, buf);
		else
			return id_mangle3("_", id, buf);
	}
	return id;
}

void firm_init_mangle(void)
{
	obstack_init(&mangle_obst);
}
