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
 * @brief   declarations for transform functions (code selection)
 * @author  Moritz Kroll, Jens Mueller
 * @version $Id$
 */
#ifndef FIRM_BE_PPC32_PPC32_TRANSFORM_H
#define FIRM_BE_PPC32_PPC32_TRANSFORM_H

void ppc32_register_transformers(void);
void ppc32_transform_node(ir_node *node, void *env);
void ppc32_transform_const(ir_node *node, void *env);

typedef enum {
	irm_Bs,
	irm_Bu,
	irm_Hs,
	irm_Hu,
	irm_Is,
	irm_Iu,
	irm_F,
	irm_D,
	irm_P,
	irm_max
} ppc32_modecode;

ppc32_modecode get_nice_modecode(ir_mode *irmode);

#endif
