/*
 * Project:     libFIRM
 * File name:   ir/ana/field_temperature.h
 * Purpose:     Compute an estimate of field temperature, i.e., field access heuristic.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:     21.7.2004
 * CVS-ID:      $Id$
 * Copyright:   (c) 2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

#ifndef _FIELD_TEMPERATURE_H_
#define _FIELD_TEMPERATURE_H_

/**
 * @file field_temperature.h
 *
 *  @author Goetz Lindenmaier
 *
 *  Watch it! This is highly java dependent.
 *
 * - All Sel nodes get an array with possibly accessed entities.
 *   (resolve polymorphy on base of inherited entities.)
 *   (the mentioned entity in first approximation.)
 *
 * - We compute a value for the entity based on the Sel nodes.
 */

#include "irnode.h"
#include "entity.h"



/** The entities that can be accessed by this Sel node. *
int     get_Sel_n_accessed_entities(ir_node *sel);
entity *get_Sel_accessed_entity    (ir_node *sel, int pos);
*/


/** Get the weighted interprocedural loop depth of the node.
    The depth is estimated by a heuristic. */
int get_weighted_loop_depth(ir_node *n);






/** An auxiliary/temporary function */
int is_jack_rts_class(type *t);

#endif /* _FIELD_TEMPERATURE_H_ */
