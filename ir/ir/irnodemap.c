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
 * @author    Matthias Braun
 * @date      30.03.2007
 * @brief     A nodemap. This should be preferred over a simple pset, because it
              tries to guarantee deterministic behavior.
 * @version   $Id$
 */
#include "config.h"

#include "irnodemap.h"
#include "irnode_t.h"
#include "hashptr.h"

static ir_nodemap_entry_t null_nodemap_entry = { NULL, NULL };

#define DO_REHASH
#define HashSet                   ir_nodemap_t
#define HashSetIterator           ir_nodemap_iterator_t
#define ValueType                 ir_nodemap_entry_t
#define NullValue                 null_nodemap_entry
#define KeyType                   ir_node*
#define ConstKeyType              const ir_node*
#define GetKey(value)             (value).node
#define InitData(self,value,key)  (value).node = (key)
#define Hash(self,key)            ((unsigned)((key)->node_nr))
#define KeysEqual(self,key1,key2) (key1) == (key2)
#define SetRangeEmpty(ptr,size)   memset(ptr, 0, (size) * sizeof((ptr)[0]))
#define EntrySetEmpty(value)      (value).node = NULL
#define EntrySetDeleted(value)    (value).node = (ir_node*) -1
#define EntryIsEmpty(value)       ((value).node == NULL)
#define EntryIsDeleted(value)     ((value).node == (ir_node*)-1)

void ir_nodemap_init_(ir_nodemap_t *self);
#define hashset_init            ir_nodemap_init_
#define hashset_init_size       ir_nodemap_init_size
#define hashset_destroy         ir_nodemap_destroy
ir_nodemap_entry_t *ir_nodemap_insert_(ir_nodemap_t *self, ir_node *node);
#define hashset_insert          ir_nodemap_insert_
#define hashset_remove          ir_nodemap_remove
ir_nodemap_entry_t *ir_nodemap_find_(const ir_nodemap_t *self,
                                     const ir_node *node);
#define hashset_find            ir_nodemap_find_
#define hashset_size            ir_nodemap_size
#define hashset_iterator_init   ir_nodemap_iterator_init
#define hashset_iterator_next   ir_nodemap_iterator_next
#define hashset_remove_iterator ir_nodemap_remove_iterator

#include "hashset.c"

void ir_nodemap_init(ir_nodemap_t *nodemap)
{
	ir_nodemap_init_size(nodemap, 16);
}

void *ir_nodemap_get(const ir_nodemap_t *self, const ir_node *node)
{
	ir_nodemap_entry_t *entry = ir_nodemap_find_(self, node);
	return entry->data;
}

void ir_nodemap_insert(ir_nodemap_t *self, ir_node *node, void *data)
{
	ir_nodemap_entry_t *entry = ir_nodemap_insert_(self, node);
	entry->data               = data;
}
