/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @author  Matthias Braun
 * @brief   Init functions for various optimisations
 */
#ifndef FIRM_OPT_INIT_H
#define FIRM_OPT_INIT_H

void firm_init_inline(void);

void firm_init_funccalls(void);

void firm_init_reassociation(void);

void firm_init_scalar_replace(void);

void firm_init_loop_opt(void);

#endif
