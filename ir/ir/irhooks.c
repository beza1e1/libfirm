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
 * @brief    Generic hooks for various libFirm functions.
 * @author   Michael Beck
 * @version  $Id$
 */
#include "config.h"

#include "irhooks.h"

/* the hooks */
hook_entry_t *hooks[hook_last];

/* register a hook */
void register_hook(hook_type_t hook, hook_entry_t *entry) {
  /* check if a hook function is specified. It's a union, so no matter which one */
  if (! entry->hook._hook_turn_into_id)
    return;

  entry->next = hooks[hook];
  hooks[hook] = entry;
}

/* unregister a hook */
void unregister_hook(hook_type_t hook, hook_entry_t *entry) {
  hook_entry_t *p;

  if (hooks[hook] == entry) {
    hooks[hook] = entry->next;
    entry->next = NULL;
    return;
  }

  for (p = hooks[hook]; p && p->next != entry; p = p->next);

  if (p) {
    p->next     = entry->next;
    entry->next = NULL;
  }
}
