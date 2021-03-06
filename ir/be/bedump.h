/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Code for dumping backend datastructures (i.e. interference graphs)
 * @author      Matthias Braun
 */
#ifndef FIRM_BE_BEDUMP_H
#define FIRM_BE_BEDUMP_H

#include <stdio.h>
#include <stdbool.h>
#include "firm_types.h"
#include "be_types.h"

/**
 * Dump interference graph
 */
void be_dump_ifg(FILE *F, ir_graph *irg, const be_ifg_t *ifg);

/**
 * Dump interference graph with affinity edges as calculated by a
 * copy-minimisation phase
 */
void be_dump_ifg_co(FILE *F, const copy_opt_t *co,
                    bool dump_costs, bool dump_colors);

/**
 * Dump the liveness information for a graph.
 * @param f The output.
 * @param irg The graph.
 */
void be_liveness_dump(FILE *F, const be_lv_t *lv);

/**
 * node_info hook that dumps liveness for blocks
 */
void be_dump_liveness_block(be_lv_t *lv, FILE *F, const ir_node *block);

#endif
