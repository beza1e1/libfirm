/* config.h.  Generated by configure.  */
/* config.h.in.  Generated from configure.in by autoheader.  */
/*
 * Project:     libFIRM
 * File name:   acconfig.h
 * Purpose:
 * Author:      Till Riedel
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2002-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/* Define to 1 if you have the <alloca.h> header file. */
/* #undef HAVE_ALLOCA_H */

/* Define to 1 if you have the <malloc.h> header file. */
#define HAVE_MALLOC_H 1

/* Define to 1 if you have the <inttypes.h> header file. */
/* #undef HAVE_INTTYPES_H */

/* Define to 1 if you have the <jni.h> header file. */
/* #undef HAVE_JNI_H */

/* Define to 1 if you have the <math.h> header file. */
#define HAVE_MATH_H 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <obstack.h> header file. */
#define HAVE_OBSTACK_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
/* #undef HAVE_STRINGS_H */

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the <io.h> header file. */
#define HAVE_IO_H 1

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
#define PACKAGE_NAME "libFIRM"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libFIRM 0.3.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libFIRM"

/* Define to the version of this package. */
#define PACKAGE_VERSION "0.3.0"

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* ---------------snip, snip ---------------------- */

/* define to enable debugging stuff. */
#define DEBUG_libfirm 1

/* define to 1 to use the libcore */
#define WITH_LIBCORE

/* define to 1 to have wchar_t support for identifiers */
#define FIRM_ENABLE_WCHAR

/* Define to disable assertion checking.  */
/* #undef NDEBUG */

/* Remove to disable inlining */
#define USE_INLINING 1

/* Define to 1 if long double works and has more range or precision than
   double. */
/* #undef HAVE_LONG_DOUBLE */

/* Define to 1 if your processor stores words with the most significant byte
   first (like Motorola and SPARC, unlike Intel and VAX). */
/* #undef WORDS_BIGENDIAN */

/* Define to 1 if Firm statistics are activated */
#define FIRM_STATISTICS

/* Define to 1 if Firm hooks are activated */
#define FIRM_ENABLE_HOOKS

/* Define to 1 if Firm inplace edges are activated */
#define FIRM_EDGES_INPLACE 1

/* Define the right volatile token */
/* #undef volatile */

/* Define the right const token */
/* #undef const */

#ifdef USE_INLINING
#define INLINE __inline
#else
#define INLINE
#endif

#define snprintf    _snprintf
#define strcasecmp  stricmp

typedef unsigned __int32 uint32_t;
typedef __int64 int64_t;
