/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
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
 * @brief     Access function for backedges -- private header.
 * @author    Goetz Lindenmaier
 * @date      7.2002
 * @version   $Id$
 */
#ifndef FIRM_ANA_IRBACKEDGE_T_H
#define FIRM_ANA_IRBACKEDGE_T_H

/**
 * Allocate a new backedge array on the obstack for given size.
 *
 * @param obst   the obstack to allocate the array on
 * @param size   the size of the backedge array
 */
bitset_t *new_backedge_arr(struct obstack *obst, size_t size);

/**
 * Adapts backedges array to new size.
 * Must be called if the in array of an IR node is changed.  Else
 * Segmentation faults might occur.
 */
void fix_backedges(struct obstack *obst, ir_node *n);

#endif
