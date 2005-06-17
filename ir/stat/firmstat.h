/*
 * Project:     libFIRM
 * File name:   ir/stat/firmstat.h
 * Purpose:     Statistics for Firm.
 * Author:      Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifndef _FIRMSTAT_H_
#define _FIRMSTAT_H_

/**
 * @file firmstat.h
 */
#include "irop.h"
#include "irnode.h"
#include "irgraph.h"

/**
 * Statistic options, can be or'ed.
 */
enum firmstat_options_t {
  FIRMSTAT_ENABLED         = 0x00000001,    /**< enable statistics */
  FIRMSTAT_PATTERN_ENABLED = 0x00000002,    /**< enable pattern calculation */
  FIRMSTAT_COUNT_STRONG_OP = 0x00000004,    /**< if set, count Mul/Div/Mod/DivMod by constant */
  FIRMSTAT_COUNT_DAG       = 0x00000008,    /**< if set, count DAG statistics */
  FIRMSTAT_COUNT_DELETED   = 0x00000010,    /**< if set, count deleted graphs */
  FIRMSTAT_COUNT_SELS      = 0x00000020,    /**< if set, count Sel(Sel(..)) differently */
  FIRMSTAT_COUNT_CONSTS    = 0x00000040,    /**< if set, count Const statistics */
  FIRMSTAT_CSV_OUTPUT      = 0x10000000     /**< CSV output of some mini-statistic */
};

/**
 * Dump a snapshot of the statistic values.
 * Never called from libFirm should be called from user.
 *
 * @param name   base name of the statistic output file
 */
void stat_dump_snapshot(const char *name);

/**
 * initialize the statistics module.
 *
 * @param enable_options  a bitmask containing the statistic options
 */
void init_stat(unsigned enable_options);

/**
 * terminates the statistics module, frees all memory
 */
void stat_term(void);

#endif /* _FIRMSTAT_H_ */
