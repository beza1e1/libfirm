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
 * @brief       Dumps global variables and constants as gas assembler.
 * @author      Christian Wuerdig, Matthias Braun
 * @date        04.11.2005
 * @version     $Id$
 */
#ifndef FIRM_BE_BEGNUAS_H
#define FIRM_BE_BEGNUAS_H

#include <stdbool.h>
#include "be.h"
#include "beemitter.h"

/**
 * Sections.
 */
typedef enum section_t {
	GAS_SECTION_TEXT,   /**< text section */
	GAS_SECTION_DATA,   /**< data section */
	GAS_SECTION_RODATA, /**< rodata section */
	GAS_SECTION_COMMON, /**< common section */
	GAS_SECTION_TLS,    /**< thread local storage section */
	GAS_SECTION_CONSTRUCTORS,   /**< ctors section */
	GAS_SECTION_DESTRUCTORS,    /**< dtors section */
	GAS_SECTION_CSTRING, /**< section for constant strings */
	GAS_SECTION_PIC_TRAMPOLINES, /**< trampolines for pic codes */
	GAS_SECTION_PIC_SYMBOLS,     /**< contains resolved pic symbols */
	GAS_SECTION_LAST = GAS_SECTION_PIC_SYMBOLS
} be_gas_section_t;

/**
 * Support for some GAS "dialects".
 */
typedef enum asm_flavour_t {
	GAS_FLAVOUR_ELF,     /**< ELF variant */
	GAS_FLAVOUR_MINGW,   /**< MinGW variant (no-ELF) */
	GAS_FLAVOUR_YASM,    /**< YASM GNU parser */
	GAS_FLAVOUR_MACH_O,  /**< Mach-O variant (as found on darwin, OS/X) */
	GAS_FLAVOUR_LAST = GAS_FLAVOUR_MACH_O
} be_gas_flavour_t;

/** The variable where the GAS dialect is stored. */
extern be_gas_flavour_t be_gas_flavour;
extern bool             be_gas_emit_types;

/**
 * Generate all entities.
 * @param main_env          the main backend environment
 * @param emit_commons      if non-zero, emit commons (non-local uninitialized entities)
 * @param only_emit_marked  if non-zero, external allocated entities that do not have
 *                          its visited flag set are ignored
 */
void be_gas_emit_decls(const be_main_env_t *main_env,
                       int only_emit_marked_entities);

/**
 * Emit an entity (the entities name or a block label)
 */
void be_gas_emit_entity(ir_entity *entity);

/**
 * Switch the current output section to the given out.
 *
 * @param section  the new output section
 */
void be_gas_emit_switch_section(be_gas_section_t section);

/**
 * emit assembler instructions necessary before starting function code
 */
void be_gas_emit_function_prolog(ir_entity *entity, unsigned alignment);

void be_gas_emit_function_epilog(ir_entity *entity);

/**
 * Return the label prefix for labeled blocks.
 */
const char *be_gas_block_label_prefix(void);

/**
 * Return the label prefix for labeled instructions.
 */
const char *be_gas_insn_label_prefix(void);

#endif
