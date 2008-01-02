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
 * @date   04.06.2007
 * @author Matthias Braun, Sebastian Hack
 * @brief  Macros to instruct the compiler compiling libFirm.
 */

#ifndef FIRM_COMPILER_H
#define FIRM_COMPILER_H

/**
 * Asserts that the constant expression x is not zero at compiletime. name has
 * to be a unique identifier.
 *
 * @note This uses the fact, that double case labels are not allowed.
 */
#define COMPILETIME_ASSERT(x, name) \
    static __attribute__((unused)) void compiletime_assert_##name (int h) { \
        switch(h) { case 0: case (x): ; } \
    }

#ifdef __GNUC__
/**
 * Indicates to the compiler that the value of x is very likely 1
 * @note Only use this in speed critical code and when you are sure x is often 1
 */
#define LIKELY(x)   __builtin_expect((x), 1)

/**
 * Indicates to the compiler that it's very likely that x is 0
 * @note Only use this in speed critical code and when you are sure x is often 0
 */
#define UNLIKELY(x) __builtin_expect((x), 0)

/**
 * Tell the compiler, that a function is pure, i.e. it only
 * uses its parameters and never modifies the "state".
 * Add this macro after the return type.
 */
#define PURE        __attribute__((const))

#else
#define LIKELY(x)   x
#define UNLIKELY(x) x
#define PURE
#endif

#endif
