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
 * @brief   PBQP data types.
 * @date    02.10.2008
 * @author  Sebastian Buchwald
 * @version $Id$
 */
#ifndef KAPS_PBQP_T_H
#define KAPS_PBQP_T_H

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "adt/obstack.h"

#define KAPS_DUMP 0
#define KAPS_ENABLE_VECTOR_NAMES 0
#define KAPS_STATISTIC 0
#define KAPS_TIMING 0
#define KAPS_USE_UNSIGNED 1

#if KAPS_USE_UNSIGNED
	typedef unsigned num;
	static const num INF_COSTS = UINT_MAX;
#else
	typedef intmax_t num;
	static const num INF_COSTS = INTMAX_MAX;
#endif

#include "matrix_t.h"
#include "vector_t.h"

typedef struct pbqp_edge_t pbqp_edge_t;
typedef struct pbqp_node_t pbqp_node_t;
typedef struct pbqp_t      pbqp_t;

struct pbqp_t {
	struct obstack obstack;            /* Obstack. */
	num            solution;           /* Computed solution. */
	size_t         num_nodes;          /* Number of PBQP nodes. */
	pbqp_node_t  **nodes;              /* Nodes of PBQP. */
	FILE          *dump_file;          /* File to dump in. */
#if KAPS_STATISTIC
	unsigned       num_bf;             /* Number of brute force reductions. */
	unsigned       num_edges;          /* Number of independent edges. */
	unsigned       num_r0;             /* Number of trivial solved nodes. */
	unsigned       num_r1;             /* Number of R1 reductions. */
	unsigned       num_r2;             /* Number of R2 reductions. */
	unsigned       num_rm;             /* Number of RM reductions. */
	unsigned       num_rn;             /* Number of RN reductions. */
#endif
};

#endif /* KAPS_PBQP_T_H */
