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
#include "be_types.h"
#include "beemitter.h"

typedef enum {
	GAS_SECTION_TEXT,            /**< text section - program code */
	GAS_SECTION_DATA,            /**< data section - arbitrary data */
	GAS_SECTION_RODATA,          /**< rodata section - read-only data */
	GAS_SECTION_BSS,             /**< bss section - zero initialized data */
	GAS_SECTION_CONSTRUCTORS,    /**< ctors section */
	GAS_SECTION_DESTRUCTORS,     /**< dtors section */
	GAS_SECTION_CSTRING,         /**< section for constant strings */
	GAS_SECTION_PIC_TRAMPOLINES, /**< trampolines for pic codes */
	GAS_SECTION_PIC_SYMBOLS,     /**< contains resolved pic symbols */
	GAS_SECTION_LAST = GAS_SECTION_PIC_SYMBOLS,
	GAS_SECTION_TYPE_MASK    = 0xFF,

	GAS_SECTION_FLAG_TLS     = 1 << 8,  /**< thread local flag */
	GAS_SECTION_FLAG_COMDAT  = 1 << 9   /**< thread local version of _BSS */
} be_gas_section_t;

typedef enum object_file_format_t {
	OBJECT_FILE_FORMAT_ELF,    /**< Executable and Linkable Format (unixes) */
	OBJECT_FILE_FORMAT_COFF,   /**< Common Object File Format (Windows) */
	OBJECT_FILE_FORMAT_MACH_O, /**< Mach Object File Format (OS/X) */
	OBJECT_FILE_FORMAT_ELF_SPARC, /**< Sparc variant of ELF */
	OBJECT_FILE_FORMAT_LAST = OBJECT_FILE_FORMAT_ELF_SPARC
} object_file_format_t;

/** The variable where the GAS dialect is stored. */
extern object_file_format_t be_gas_object_file_format;
extern bool                 be_gas_emit_types;

/**
 * the .type directive needs to specify @function, #function or %function
 * depending on the target architecture (yay)
 */
extern char                 be_gas_elf_type_char;

/**
 * Generate all entities.
 * @param main_env          the main backend environment
 */
void be_gas_emit_decls(const be_main_env_t *main_env);

/**
 * Switch the current output section to the given out.
 *
 * @param section  the new output section
 */
void be_gas_emit_switch_section(be_gas_section_t section);

/**
 * emit assembler instructions necessary before starting function code
 */
void be_gas_emit_function_prolog(const ir_entity *entity,
                                 unsigned po2alignment);

void be_gas_emit_function_epilog(const ir_entity *entity);

char const *be_gas_get_private_prefix(void);

/**
 * emit ld_ident of an entity and performs additional mangling if necessary.
 * (mangling is necessary for ir_visibility_private for example).
 * Emits a block label for type_code entities.
 */
void be_gas_emit_entity(const ir_entity *entity);

/**
 * Emit (a private) symbol name for a firm block
 */
void be_gas_emit_block_name(const ir_node *block);

/**
 * Return the label prefix for labeled instructions.
 */
const char *be_gas_insn_label_prefix(void);

#endif
