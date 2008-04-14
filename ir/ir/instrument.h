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
 * @brief   Instrumentation of graphs.
 * @version $Id: $
 */
#ifndef FIRM_IR_INSTRUMENT_H
#define FIRM_IR_INSTRUMENT_H

/**
 * Adds a Call at the beginning of the given irg.
 *
 * @param irg  the graph to instrument
 * @param ent  the entity to call
 */
void instrument_initcall(ir_graph *irg, ir_entity *ent);

#endif
