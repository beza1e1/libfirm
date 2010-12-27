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
 * @brief   convenience layer over raw_bitsets (stores number of bits
 *          with the bitfield)
 * @author  Matthias Braun
 * @version $Id$
 */
#ifndef FIRM_ADT_BITSET_H
#define FIRM_ADT_BITSET_H

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "xmalloc.h"
#include "bitfiddle.h"
#include "raw_bitset.h"

typedef struct bitset_t {
	size_t size;       /**< size of the bitset in bits */
	unsigned data[1];  /**< data (should be declared data[] but this is only
	                        allowed in C99) */
} bitset_t;

/**
 * return the number of bytes a bitset would need
 */
static inline size_t bitset_total_size(size_t n_bits)
{
	return sizeof(bitset_t) - sizeof(((bitset_t*)0)->data)
		+ BITSET_SIZE_BYTES(n_bits);
}

/**
 * initialize a bitset for bitsize size (bitset should point to memory
 * with a size calculated by bitset_total_size)
 */
static inline bitset_t *bitset_init(void *memory, size_t size)
{
	bitset_t *result = (bitset_t*) memory;
	result->size = size;
	rbitset_clear_all(result->data, size);
	return result;
}

/**
 * Allocate a bitset on an obstack.
 * @param obst The obstack.
 * @param size The greatest bit that shall be stored in the set.
 * @return A pointer to an empty initialized bitset.
 */
static inline bitset_t *bitset_obstack_alloc(struct obstack *obst,
                                             size_t n_bits)
{
	size_t size   = bitset_total_size(n_bits);
	void  *memory = obstack_alloc(obst, size);
	return bitset_init(memory, n_bits);
}

/**
 * Allocate a bitset via malloc.
 * @param size The greatest bit that shall be stored in the set.
 * @return A pointer to an empty initialized bitset.
 */
static inline bitset_t *bitset_malloc(size_t n_bits)
{
	size_t  size   = bitset_total_size(n_bits);
	void   *memory = xmalloc(size);
	return bitset_init(memory, n_bits);
}

/**
 * Free a bitset allocated with bitset_malloc().
 * @param bs The bitset.
 */
static inline void bitset_free(bitset_t *bitset)
{
	xfree(bitset);
}

/**
 * Allocate a bitset on the stack via alloca.
 * @param size The greatest bit that shall be stored in the set.
 * @return A pointer to an empty initialized bitset.
 */
#define bitset_alloca(size) \
	bitset_init(alloca(bitset_total_size(size)), (size))

/**
 * Get the size of the bitset in bits.
 * @note Note the difference between capacity and size.
 * @param bs The bitset.
 * @return The highest bit which can be set or cleared plus 1.
 */
static inline size_t bitset_size(const bitset_t *bitset)
{
	return bitset->size;
}

/**
 * Set a bit in the bitset.
 * @param bs The bitset.
 * @param bit The bit to set.
 */
static inline void bitset_set(bitset_t *bs, size_t bit)
{
	assert(bit < bs->size);
	rbitset_set(bs->data, bit);
}

/**
 * Clear a bit in the bitset.
 * @param bs The bitset.
 * @param bit The bit to clear.
 */
static inline void bitset_clear(bitset_t *bs, size_t bit)
{
	assert(bit < bs->size);
	rbitset_clear(bs->data, bit);
}

/**
 * Check, if a bit is set.
 * @param bs The bitset.
 * @param bit The bit to check for.
 * @return 1, if the bit was set, 0 if not.
 */
static inline bool bitset_is_set(const bitset_t *bs, size_t bit)
{
	assert(bit < bs->size);
	return rbitset_is_set(bs->data, bit);
}

/**
 * Flip a bit in a bitset.
 * @param bs The bitset.
 * @param bit The bit to flip.
 */
static inline void bitset_flip(bitset_t *bs, size_t bit)
{
	assert(bit < bs->size);
	rbitset_flip(bs->data, bit);
}

/**
 * Flip the whole bitset.
 * @param bs The bitset.
 */
static inline void bitset_flip_all(bitset_t *bs)
{
	rbitset_flip_all(bs->data, bs->size);
}

/**
 * Copy a bitset to another. Both bitset must be initialized and have the same
 * number of bits.
 * @param tgt The target bitset.
 * @param src The source bitset.
 * @return The target bitset.
 */
static inline void bitset_copy(bitset_t *tgt, const bitset_t *src)
{
	assert(tgt->size == src->size);
	rbitset_copy(tgt->data, src->data, src->size);
}

static inline void bitset_copy_into(bitset_t *tgt, const bitset_t *src)
{
	assert(tgt->size >= src->size);
	rbitset_copy_into(tgt->data, src->data, src->size);
}

/**
 * Find the next unset bit from a given bit.
 * @note Note that if pos is unset, pos is returned.
 * @param bs The bitset.
 * @param pos The bit from which to search for the next set bit.
 * @return The next set bit from pos on, or (size_t)-1, if no unset bit was
 * found after pos.
 */
static inline size_t bitset_next_clear(const bitset_t *bs, size_t pos)
{
	if (pos >= bs->size)
		return (size_t)-1;
	return rbitset_next_max(bs->data, pos, bs->size, false);
}

/**
 * Find the next set bit from a given bit.
 * @note Note that if pos is set, pos is returned.
 * @param bs The bitset.
 * @param pos The bit from which to search for the next set bit.
 * @return The next set bit from pos on, or (size_t)-1, if no set bit was
 * found after pos.
 */
static inline size_t bitset_next_set(const bitset_t *bs, size_t pos)
{
	if (pos >= bs->size)
		return (size_t)-1;
	return rbitset_next_max(bs->data, pos, bs->size, true);
}

/**
 * Convenience macro for bitset iteration.
 * @param bitset The bitset.
 * @param elm A size_t variable.
 */
#define bitset_foreach(bitset,elm) \
	for(elm = bitset_next_set(bitset,0); elm != (size_t)-1; elm = bitset_next_set(bitset,elm+1))


#define bitset_foreach_clear(bitset,elm) \
	for(elm = bitset_next_clear(bitset,0); elm != (size_t) -1; elm = bitset_next_clear(bitset,elm+1))

/**
 * Count the bits set.
 * This can also be seen as the cardinality of the set.
 * @param bs The bitset.
 * @return The number of bits set in the bitset.
 */
static inline size_t bitset_popcount(const bitset_t *bs)
{
	return rbitset_popcount(bs->data, bs->size);
}

/**
 * Clear the bitset.
 * This sets all bits to zero.
 * @param bs The bitset.
 */
static inline void bitset_clear_all(bitset_t *bs)
{
	rbitset_clear_all(bs->data, bs->size);
}

/**
 * Set the bitset.
 * This sets all bits to one.
 * @param bs The bitset.
 */
static inline void bitset_set_all(bitset_t *bs)
{
	rbitset_set_all(bs->data, bs->size);
}

/**
 * Check, if one bitset is contained by another.
 * That is, each bit set in lhs is also set in rhs.
 * @param lhs A bitset.
 * @param rhs Another bitset.
 * @return 1, if all bits in lhs are also set in rhs, 0 otherwise.
 */
static inline bool bitset_contains(const bitset_t *lhs, const bitset_t *rhs)
{
	assert(lhs->size == rhs->size);
	return rbitset_contains(lhs->data, rhs->data, lhs->size);
}

/**
 * Treat the bitset as a number and subtract 1.
 * @param bs The bitset.
 * @return The same bitset.
 */
static inline void bitset_minus1(bitset_t *bs)
{
	rbitset_minus1(bs->data, bs->size);
}

/**
 * Check if two bitsets intersect.
 * @param a The first bitset.
 * @param b The second bitset.
 * @return 1 if they have a bit in common, 0 if not.
 */
static inline bool bitset_intersect(const bitset_t *a, const bitset_t *b)
{
	assert(a->size == b->size);
	return rbitsets_have_common(a->data, b->data, a->size);
}

/**
 * set or clear all bits in the range [from;to[.
 * @param a      The bitset.
 * @param from   The first index to set to one.
 * @param to     The last index plus one to set to one.
 * @param do_set If 1 the bits are set, if 0, they are cleared.
 */
static inline void bitset_mod_range(bitset_t *a, size_t from, size_t to,
                                    bool do_set)
{
	if (from == to)
	    return;

	if (to < from) {
		size_t tmp = from;
		from = to;
		to = tmp;
	}

	if (to > a->size)
		to = a->size;

	rbitset_set_range(a->data, from, to, do_set);
}

#define bitset_set_range(bs, from, to)   bitset_mod_range((bs), (from), (to), 1)
#define bitset_clear_range(bs, from, to) bitset_mod_range((bs), (from), (to), 0)

/**
 * Check, if a bitset is empty.
 * @param a The bitset.
 * @return 1, if the bitset is empty, 0 if not.
 */
static inline bool bitset_is_empty(const bitset_t *bs)
{
	return rbitset_is_empty(bs->data, bs->size);
}

/**
 * Print a bitset to a stream.
 * The bitset is printed as a comma separated list of bits set.
 * @param file The stream.
 * @param bs The bitset.
 */
static inline void bitset_fprint(FILE *file, const bitset_t *bs)
{
	const char *prefix = "";
	size_t i;

	putc('{', file);
	for(i = bitset_next_set(bs, 0); i != (size_t)-1; i = bitset_next_set(bs, i + 1)) {
		fprintf(file, "%s%d", prefix, i);
		prefix = ",";
	}
	putc('}', file);
}

/**
 * Perform tgt = tgt & src operation.
 * @param tgt  The target bitset.
 * @param src  The source bitset.
 * @return the tgt set.
 */
static inline void bitset_and(bitset_t *tgt, const bitset_t *src)
{
	assert(tgt->size == src->size);
	rbitset_and(tgt->data, src->data, src->size);
}

/**
 * Perform tgt = tgt & ~src operation.
 * @param tgt  The target bitset.
 * @param src  The source bitset.
 * @return the tgt set.
 */
static inline void bitset_andnot(bitset_t *tgt, const bitset_t *src)
{
	assert(tgt->size == src->size);
	rbitset_andnot(tgt->data, src->data, src->size);
}

/**
 * Perform Union, tgt = tgt u src operation.
 * @param tgt  The target bitset.
 * @param src  The source bitset.
 * @return the tgt set.
 */
static inline void bitset_or(bitset_t *tgt, const bitset_t *src)
{
	assert(tgt->size == src->size);
	rbitset_or(tgt->data, src->data, src->size);
}

/**
 * Perform tgt = tgt ^ src operation.
 * @param tgt  The target bitset.
 * @param src  The source bitset.
 * @return the tgt set.
 */
static inline void bitset_xor(bitset_t *tgt, const bitset_t *src)
{
	assert(tgt->size == src->size);
	rbitset_xor(tgt->data, src->data, src->size);
}

/**
 * Copy a raw bitset into an bitset.
 */
static inline void rbitset_copy_to_bitset(const unsigned *rbitset,
                                          bitset_t *bitset)
{
	rbitset_copy(bitset->data, rbitset, bitset->size);
}

#endif
