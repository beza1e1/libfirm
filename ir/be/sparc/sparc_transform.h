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
 * @brief   declaration for the transform function (code selection)
 * @version $Id: TEMPLATE_transform.h 26542 2009-09-18 09:18:32Z matze $
 */
#ifndef FIRM_BE_SPARC_SPARC_TRANSFORM_H
#define FIRM_BE_SPARC_SPARC_TRANSFORM_H

void sparc_init_transform(void);

void sparc_register_transformers(void);

void sparc_transform_graph(sparc_code_gen_t *cg);
#endif