/* Copyright (c) 2002 by Universit�t Karlsruhe (TH).  All Rights Reserved */

/*
   NAME
     bs
   PURPOSE
     provide bs_t
   S
     not quite complete
   HISTORY
     liekweg - Feb 27, 2002: Created.
   CVS:
     $Id$
 */

# ifndef _BS_H_
# define _BS_H_

/**
 * the type of a bit set
 */
typedef long int bs_t;

/** set bit in a bit set */
# define bs_set(bs, i) (bs) |= (0x00000001 << i)

/** get bit in a bit set */
# define bs_get(bs, i) (bs) &  (0x00000001 << i)

/** logical AND of two bit sets */
# define bs_and(bsa, bsb) (bsa) &= (bsb)

/** logical OR of two bit sets */
# define bs_or(bsa, bsb)  (bsa) |= (bsb)

/** logical XOR of two bit sets */
# define bs_xor(bsa, bsb) (bsa) ^= (bsb)

/** returns TRUE if at least one bit is set */
# define bs_zro(bs) (0x00000000 != bs)

# endif /* ndef _BS_H_ */
