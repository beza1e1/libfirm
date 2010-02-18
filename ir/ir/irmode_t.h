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
 * @brief   Data modes of operations -- private header.
 * @author  Martin Trapp, Christian Schaefer, Goetz Lindenmaier, Mathias Heil,
 *          Michael Beck
 * @version $Id$
 */
#ifndef FIRM_IR_IRMODE_T_H
#define FIRM_IR_IRMODE_T_H

#include <assert.h>
#include "irtypes.h"
#include "irmode.h"

/* ------------------------------- *
 * inline functions                *
 * ------------------------------- */
static inline ir_mode *_get_modeP_code(void) { return mode_P_code; }

static inline ir_mode *_get_modeP_data(void) { return mode_P_data; }

static inline ident *_get_mode_ident(const ir_mode *mode) { return mode->name; }

static inline ir_mode_sort _get_mode_sort(const ir_mode *mode) { return mode->sort; }

static inline unsigned _get_mode_size_bits(const ir_mode *mode) { return mode->size; }

static inline unsigned _get_mode_size_bytes(const ir_mode *mode)
{
	unsigned size = _get_mode_size_bits(mode);
	if ((size & 7) != 0) return (unsigned) -1;
	return size >> 3;
}

static inline int _get_mode_sign(const ir_mode *mode) { return mode->sign; }

static inline ir_mode_arithmetic _get_mode_arithmetic(const ir_mode *mode) { return mode->arithmetic; }

static inline unsigned int _get_mode_modulo_shift(const ir_mode *mode) { return mode->modulo_shift; }

static inline unsigned int _get_mode_vector_elems(const ir_mode *mode) { return mode->vector_elem; }

static inline void * _get_mode_link(const ir_mode *mode) { return mode->link; }

static inline void _set_mode_link(ir_mode *mode, void *l) { mode->link = l; }

/* Functions to check, whether a mode is signed, float, int, num, data,
   datab or dataM. For more exact definitions read the corresponding pages
   in the firm documentation or the following enumeration

   The set of "float" is defined as:
   ---------------------------------
   float = {irm_F, irm_D, irm_E}

   The set of "int" is defined as:
   -------------------------------
   int   = {irm_Bs, irm_Bu, irm_Hs, irm_Hu, irm_Is, irm_Iu, irm_Ls, irm_Lu}

   The set of "num" is defined as:
   -------------------------------
   num   = {irm_F, irm_D, irm_E, irm_Bs, irm_Bu, irm_Hs, irm_Hu,
            irm_Is, irm_Iu, irm_Ls, irm_Lu}
            = {float || int}

   The set of "data" is defined as:
   -------------------------------
   data  = {irm_F, irm_D, irm_E irm_Bs, irm_Bu, irm_Hs, irm_Hu,
            irm_Is, irm_Iu, irm_Ls, irm_Lu, irm_C, irm_U, irm_P}
            = {num || irm_C || irm_U || irm_P}

   The set of "datab" is defined as:
   ---------------------------------
   datab = {irm_F, irm_D, irm_E, irm_Bs, irm_Bu, irm_Hs, irm_Hu,
            irm_Is, irm_Iu, irm_Ls, irm_Lu, irm_C, irm_U, irm_P, irm_b}
            = {data || irm_b }

   The set of "dataM" is defined as:
   ---------------------------------
   dataM = {irm_F, irm_D, irm_E, irm_Bs, irm_Bu, irm_Hs, irm_Hu,
            irm_Is, irm_Iu, irm_Ls, irm_Lu, irm_C, irm_U, irm_P, irm_M}
            = {data || irm_M}
*/

static inline int _mode_is_signed(const ir_mode *mode)
{
	return mode->sign;
}

static inline int _mode_is_float(const ir_mode *mode)
{
	return (_get_mode_sort(mode) == irms_float_number);
}

static inline int _mode_is_int(const ir_mode *mode)
{
	return (_get_mode_sort(mode) == irms_int_number);
}

static inline int _mode_is_reference(const ir_mode *mode)
{
	return (_get_mode_sort(mode) == irms_reference);
}

static inline int _mode_is_num(const ir_mode *mode)
{
	return (_get_mode_sort(mode) & irmsh_is_num);
}

static inline int _mode_is_data(const ir_mode *mode)
{
	return (_get_mode_sort(mode) & irmsh_is_data);
}

static inline int _mode_is_datab(const ir_mode *mode)
{
	return (_get_mode_sort(mode) & irmsh_is_datab);
}

static inline int _mode_is_dataM(const ir_mode *mode)
{
	return (_get_mode_sort(mode) & irmsh_is_dataM);
}

static inline int _mode_is_float_vector(const ir_mode *mode)
{
	return (_get_mode_sort(mode) == irms_float_number) && (_get_mode_vector_elems(mode) > 1);
}

static inline int _mode_is_int_vector(const ir_mode *mode)
{
	return (_get_mode_sort(mode) == irms_int_number) && (_get_mode_vector_elems(mode) > 1);
}

/** mode module initialization, call once before use of any other function **/
void init_mode(void);

/** mode module finalization. frees all memory.  */
void finish_mode(void);

#define get_modeP_code()               _get_modeP_code()
#define get_modeP_data()               _get_modeP_data()
#define get_mode_ident(mode)           _get_mode_ident(mode)
#define get_mode_sort(mode)            _get_mode_sort(mode)
#define get_mode_size_bits(mode)       _get_mode_size_bits(mode)
#define get_mode_size_bytes(mode)      _get_mode_size_bytes(mode)
#define get_mode_sign(mode)            _get_mode_sign(mode)
#define get_mode_arithmetic(mode)      _get_mode_arithmetic(mode)
#define get_mode_modulo_shift(mode)    _get_mode_modulo_shift(mode)
#define get_mode_n_vector_elems(mode)  _get_mode_vector_elems(mode)
#define get_mode_link(mode)            _get_mode_link(mode)
#define set_mode_link(mode, l)         _set_mode_link(mode, l)
#define mode_is_signed(mode)           _mode_is_signed(mode)
#define mode_is_float(mode)            _mode_is_float(mode)
#define mode_is_int(mode)              _mode_is_int(mode)
#define mode_is_reference(mode)        _mode_is_reference(mode)
#define mode_is_num(mode)              _mode_is_num(mode)
#define mode_is_data(mode)             _mode_is_data(mode)
#define mode_is_datab(mode)            _mode_is_datab(mode)
#define mode_is_dataM(mode)            _mode_is_dataM(mode)
#define mode_is_float_vector(mode)     _mode_is_float_vector(mode)
#define mode_is_int_vector(mode)       _mode_is_int_vector(mode)

#endif
