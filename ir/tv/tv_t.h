/* Declarations for Target Values.
   Copyright (C) 1995, 1996 Christian von Roques */

/**
* @file tv_t.h
*
* @author Christian von Roques
*/

/* $Id$ */

#ifndef _TV_T_H_
#define _TV_T_H_

#include "tv.h"
#include "xprintf.h"

/****s* tv/tarval
 *
 * NAME
 *    tarval
 *   This struct represents the aforementioned tarvals.
 *
 * DESCRIPTION
 *    A tarval struct consists of an internal representation of the
 *   value and some additional fields further describing the value.
 *
 * ATTRIBUTES
 *   ir_mode *mode     The mode of the stored value
 *   void *value       The internal representation
 *
 * SEE ALSO
 *   irmode.h for predefined modes
 *
 ******/

struct tarval {
    ir_mode *mode; /* mode of the stored value */
    const void *value; /* the value stored in an internal way... */
    unsigned int length; /* the length of the stored value */
};

/* xfprint output */
int tarval_print (XP_PAR1, const xprintf_info *, XP_PARN);

/** remove tarval representing an entity that is about to be destroyed */
void free_tarval_entity(entity *ent);

#endif /* _TV_T_H_ */
