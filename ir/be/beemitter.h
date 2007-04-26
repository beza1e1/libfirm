/*
 * Author:      Matthias Braun
 * Date:		12.03.2007
 * Copyright:   (c) Universitaet Karlsruhe
 * License:     This file is protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifndef FIRM_BE_BEEMITTER_H
#define FIRM_BE_BEEMITTER_H

#include <stdio.h>
#include <stdarg.h>
#include "obst.h"
#include "ident.h"
#include "irnode.h"
#include "be.h"

/* framework for emitting data (usually the final assembly code) */

/** The emitter environment. */
typedef struct be_emit_env_t {
	FILE           *F;         /**< The handle of the (assembler) file that is written to. */
	struct obstack obst;       /**< An obstack for temporary storage. */
	int            linelength; /**< The length of the current line. */
} be_emit_env_t;

/**
 * Emit a character to the (assembler) output.
 *
 * @param env  the emitter environment
 */
static INLINE void be_emit_char(be_emit_env_t *env, char c) {
	obstack_1grow(&env->obst, c);
	env->linelength++;
}

/**
 * Emit a string to the (assembler) output.
 *
 * @param env  the emitter environment
 * @param str  the string
 * @param l    the length of the given string
 */
static INLINE void be_emit_string_len(be_emit_env_t *env, const char *str,
                                      size_t l)
{
	obstack_grow(&env->obst, str, l);
	env->linelength += l;
}

/**
 * Emit a null-terminated string to the (assembler) output.
 *
 * @param env  the emitter environment
 * @param str  the null-terminated string
 */
static INLINE void be_emit_string(be_emit_env_t *env, const char *str)
{
	size_t len = strlen(str);
	be_emit_string_len(env, str, len);
}

/**
 * Emit a C string-constant to the (assembler) output.
 *
 * @param env  the emitter environment
 * @param str  the null-terminated string constant
 */
#define be_emit_cstring(env, str) { be_emit_string_len(env, str, sizeof(str)-1); }

/**
 * Initializes an emitter environment.
 *
 * @param env  the (uninitialized) emitter environment
 * @param F    a file handle where the emitted file is written to.
 */
void be_emit_init_env(be_emit_env_t *env, FILE *F);

/**
 * Destroys the given emitter environment.
 *
 * @param env  the emitter environment
 */
void be_emit_destroy_env(be_emit_env_t *env);

/**
 * Emit an ident to the (assembler) output.
 *
 * @param env  the emitter environment
 * @param id   the ident to be emitted
 */
void be_emit_ident(be_emit_env_t *env, ident *id);

/**
 * Emit the output of an ir_printf.
 *
 * @param env  the emitter environment
 * @param fmt  the ir_printf format
 */
void be_emit_irprintf(be_emit_env_t *env, const char *fmt, ...);

/**
 * Emit the output of an ir_vprintf.
 *
 * @param env  the emitter environment
 * @param fmt  the ir_printf format
 */
void be_emit_irvprintf(be_emit_env_t *env, const char *fmt, va_list args);

/**
 * Flush the line in the current line buffer to the emitter file.
 *
 * @param env  the emitter environment
 */
void be_emit_write_line(be_emit_env_t *env);

/**
 * Flush the line in the current line buffer to the emitter file and
 * appends a gas-style comment with the node number and writes the line
 *
 * @param env   the emitter environment
 * @param node  the node to get the debug info from
 */
void be_emit_finish_line_gas(be_emit_env_t *env, const ir_node *node);

/**
 * Emit spaces until the comment position is reached.
 *
 * @param env  the emitter environment
 */
void be_emit_pad_comment(be_emit_env_t *env);

#endif /* FIRM_BE_BEEMITTER_H */
