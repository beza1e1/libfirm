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
 * @brief   Definition of the Firm IR base types, concentrated here
 * @author  Michael Beck
 * @version $Id$
 */
#ifndef FIRM_IR_IRDEFS_H
#define FIRM_IR_IRDEFS_H

#include "firm_types.h"
#include "irdom_t.h"
#include "irmode.h"
#include "irnode.h"
#include "irgraph.h"
#include "iredgekinds.h"
#include "irtypeinfo.h"
#include "irextbb.h"
#include "execution_frequency.h"
#include "irmemory.h"
#include "callgraph.h"
#include "irprog.h"
#include "field_temperature.h"
#include "irphase.h"

#include "pset.h"
#include "set.h"
#include "list.h"
#include "obst.h"
#include "vrp.h"

/**
 * List of phases. (We will add a register/unregister interface if managing
 * this gets too tedious)
 */
typedef enum ir_phase_id {
	PHASE_VRP,
	PHASE_LAST = PHASE_VRP
} ir_phase_id;

/** The type of an ir_op. */
struct ir_op {
	unsigned code;          /**< The unique opcode of the op. */
	ident *name;            /**< The name of the op. */
	size_t attr_size;       /**< Space needed in memory for private attributes. */
	op_pin_state pin_state; /**< How to deal with the node in CSE, PRE. */
	op_arity opar;          /**< The arity of operator. */
	int op_index;           /**< The index of the first data operand, 0 for most cases, 1 for Div etc. */
	unsigned flags;         /**< Flags describing the behavior of the ir_op, a bitmasks of irop_flags. */
	unsigned tag;           /**< Some custom TAG value the op's creator set to. */
	void *attr;             /**< custom pointer where op's creator can attach attribute stuff to. */

	ir_op_ops ops;          /**< The operations of the this op. */
};

/**
 * Contains relevant information about a mode.
 *
 * Necessary information about a mode is stored in this struct
 * which is used by the tarval module to perform calculations
 * and comparisons of values of a such described mode.
 *
 * ATTRIBUTES:
 *  -  ident *name:             Name of this mode. Two modes are different if the name is different.
 *  -  ir_mode_sort sort:       sort of mode specifying possible usage categories
 *  -  int    size:             size of the mode in Bits.
 *  -  unsigned sign:1:         signedness of this mode
 *  -  ... more to come
 *  -  modulo_shift             specifies for modes of kind irms_int_number
 *                              whether shift applies modulo to value of bits to shift
 *
 * SEE ALSO:
 *    The tech report 1999-44 describing FIRM and predefined modes
 *    tarval.h
 */
struct ir_mode {
	firm_kind         kind;       /**< distinguishes this node from others */
	ident             *name;      /**< Name ident of this mode */
	ir_type           *type;      /**< corresponding primitive type */

	/* ----------------------------------------------------------------------- */
	/* On changing this struct you have to evaluate the mode_are_equal function!*/
	ir_mode_sort      sort;          /**< coarse classification of this mode:
                                          int, float, reference ...
                                          (see irmode.h) */
	ir_mode_arithmetic
	                  arithmetic;    /**< different arithmetic operations possible with a mode */
	unsigned          size;          /**< size of the mode in Bits. */
	unsigned          sign:1;        /**< signedness of this mode */
	unsigned int      modulo_shift;  /**< number of bits a values of this mode will be shifted */
	unsigned          vector_elem;   /**< if this is not equal 1, this is a vector mode with
                                          vector_elem number of elements, size contains the size
                                          of all bits and must be dividable by vector_elem */

	/* ----------------------------------------------------------------------- */
	tarval            *min;         /**< the minimum value that can be expressed */
	tarval            *max;         /**< the maximum value that can be expressed */
	tarval            *null;        /**< the value 0 */
	tarval            *one;         /**< the value 1 */
	tarval            *minus_one;   /**< the value -1 */
	tarval            *all_one;     /**< the value ~0 */
	ir_mode           *eq_signed;   /**< For pointer modes, the equivalent signed integer one. */
	ir_mode           *eq_unsigned; /**< For pointer modes, the equivalent unsigned integer one. */
	void              *link;        /**< To store some intermediate information */
	const void        *tv_priv;     /**< tarval module will save private data here */
};

/* ir node attributes */

/** first attribute of Bad, Block, Anchor nodes */
typedef struct {
	ir_graph *irg;              /**< The graph this block like node belongs to. */
} irg_attr;

typedef struct {
	irg_attr    irg;
} bad_attr;

/** Block attributes */
typedef struct {
	/* General attributes */
	irg_attr     irg;           /**< The graph this block belongs to. */
	ir_visited_t block_visited; /**< For the walker that walks over all blocks. */
	/* Attributes private to construction: */
	unsigned is_matured:1;      /**< If set, all in-nodes of the block are fixed. */
	unsigned is_dead:1;         /**< If set, the block is dead (and could be replace by a Bad. */
	unsigned marked:1;          /**< Can be set/unset to temporary mark a block. */
	ir_node **graph_arr;        /**< An array to store all parameters. */
	/* Attributes holding analyses information */
	ir_dom_info dom;            /**< Datastructure that holds information about dominators.
	                                 @@@ @todo
	                                 Eventually overlay with graph_arr as only valid
	                                 in different phases.  Eventually inline the whole
	                                 datastructure. */
	ir_dom_info pdom;           /**< Datastructure that holds information about post-dominators. */
	ir_node ** in_cg;           /**< array with predecessors in
	                             * interprocedural_view, if they differ
	                             * from intraprocedural predecessors */
	unsigned *backedge;         /**< Raw Bitfield n set to true if pred n is backedge.*/
	unsigned *cg_backedge;      /**< Raw Bitfield n set to true if pred n is interprocedural backedge. */
	ir_extblk *extblk;          /**< The extended basic block this block belongs to. */
	ir_region *region;          /**< The immediate structural region this block belongs to. */
	ir_entity *entity;          /**< entitiy representing this block */
	ir_node  *phis;             /**< The list of Phi nodes in this block. */

	struct list_head succ_head; /**< A list head for all successor edges of a block. */
} block_attr;

/** Cond attributes. */
typedef struct {
	long default_proj;           /**< only for non-binary Conds: biggest Proj number, i.e. the one used for default. */
	cond_jmp_predicate jmp_pred; /**< only for binary Conds: The jump predication. */
} cond_attr;

/** Const attributes. */
typedef struct {
	tarval  *tarval;  /**< the target value */
} const_attr;

/** SymConst attributes. */
typedef struct {
	symconst_symbol sym;  // old tori
	symconst_kind   kind;
} symconst_attr;

/** Sel attributes. */
typedef struct {
	ir_entity *entity;    /**< entity to select */
} sel_attr;

/** Exception attributes. */
typedef struct {
	op_pin_state   pin_state;     /**< the pin state for operations that might generate a exception:
									 If it's know that no exception will be generated, could be set to
									 op_pin_state_floats. */
} except_attr;

/** Call attributes. */
typedef struct {
	except_attr exc;               /**< the exception attribute. MUST be the first one. */
	ir_type     *type;             /**< type of called procedure */
	ir_entity   **callee_arr;      /**< result of callee analysis */
	unsigned    tail_call:1;       /**< if set, can be a tail call */
} call_attr;

/** Builtin attributes. */
typedef struct {
	except_attr     exc;           /**< the exception attribute. MUST be the first one. */
	ir_builtin_kind kind;          /**< kind of the called builtin procedure */
	ir_type         *type;         /**< type of called builtin procedure */
} builtin_attr;

/** Alloc attributes. */
typedef struct {
	except_attr    exc;           /**< the exception attribute. MUST be the first one. */
    ir_where_alloc where;         /**< stack, heap or other managed part of memory */
	ir_type        *type;         /**< Type of the allocated object.  */
} alloc_attr;

/** Free attributes. */
typedef struct {
	ir_type *type;                /**< Type of the allocated object.  */
	ir_where_alloc where;         /**< stack, heap or other managed part of memory */
} free_attr;

/** InstOf attributes. */
typedef struct {
	except_attr    exc;           /**< the exception attribute. MUST be the first one. */
	ir_type *type;                /**< the type of which the object pointer must be */
} io_attr;

/** Cast attributes. */
typedef struct {
	ir_type *type;                /**< Type of the casted node. */
} cast_attr;

/** Load attributes. */
typedef struct {
	except_attr   exc;            /**< The exception attribute. MUST be the first one. */
    unsigned      volatility:1;   /**< The volatility of this Load operation. */
    unsigned      aligned:1;      /**< The align attribute of this Load operation. */
	ir_mode       *mode;          /**< The mode of this Load operation. */
} load_attr;

/** Store attributes. */
typedef struct {
	except_attr   exc;            /**< the exception attribute. MUST be the first one. */
	unsigned      volatility:1;   /**< The volatility of this Store operation. */
	unsigned      aligned:1;      /**< The align attribute of this Store operation. */
} store_attr;

typedef struct {
	ir_node        *next;         /**< Points to the next Phi in the Phi list of a block. */
	union {
		unsigned       *backedge;     /**< Raw Bitfield: bit n is set to true if pred n is backedge. */
		int            pos;           /**< For Phi0. Used to remember the value defined by
		                               this Phi node.  Needed when the Phi is completed
		                               to call get_r_internal_value() to find the
		                               predecessors. If this attribute is set, the Phi
		                               node takes the role of the obsolete Phi0 node,
		                               therefore the name. */
	} u;
} phi_attr;


/**< Confirm attribute. */
typedef struct {
	pn_Cmp cmp;                   /**< The compare operation. */
} confirm_attr;

/** CopyB attribute. */
typedef struct {
	except_attr    exc;           /**< The exception attribute. MUST be the first one. */
	ir_type        *type;         /**< Type of the copied entity. */
} copyb_attr;

/** Bound attribute. */
typedef struct {
	except_attr exc;              /**< The exception attribute. MUST be the first one. */
} bound_attr;

/** Conv attribute. */
typedef struct {
	char           strict;        /**< If set, this is a strict Conv that cannot be removed. */
} conv_attr;

/** Div/Mod/DivMod/Quot attribute. */
typedef struct {
	except_attr    exc;           /**< The exception attribute. MUST be the first one. */
	ir_mode        *resmode;      /**< Result mode for the division. */
	char           no_remainder;  /**< Set, if known that a division can be done without a remainder. */
} divmod_attr;

/** Inline Assembler support attribute. */
typedef struct {
	/* BEWARE: pin state MUST be the first attribute */
	op_pin_state      pin_state;            /**< the pin state for operations that might generate a exception */
	ident             *text;                /**< The inline assembler text. */
	ir_asm_constraint *input_constraints;   /**< Input constraints. */
	ir_asm_constraint *output_constraints;  /**< Output constraints. */
	ident             **clobbers;           /**< List of clobbered registers. */
} asm_attr;

/** Some IR-nodes just have one attribute, these are stored here,
   some have more. Their name is 'irnodename_attr' */
typedef union {
	irg_attr       irg;           /**< For Blocks and Bad: its belonging irg */
	bad_attr       bad;           /**< for Bads: irg reference */
	block_attr     block;         /**< For Block: Fields needed to construct it */
	cond_attr      cond;          /**< For Cond. */
	const_attr     con;           /**< For Const: contains the value of the constant and a type */
	symconst_attr  symc;          /**< For SymConst. */
	sel_attr       sel;           /**< For Sel. */
	call_attr      call;          /**< For Call. */
	builtin_attr   builtin;       /**< For Builtin. */
	alloc_attr     alloc;         /**< For Alloc. */
	free_attr      free;          /**< For Free. */
	io_attr        instof;        /**< For InstOf */
	cast_attr      cast;          /**< For Cast. */
	load_attr      load;          /**< For Load. */
	store_attr     store;         /**< For Store. */
	phi_attr       phi;           /**< For Phi. */
	long           proj;          /**< For Proj: contains the result position to project */
	confirm_attr   confirm;       /**< For Confirm: compare operation and region. */
	except_attr    except;        /**< For Phi node construction in case of exceptions */
	copyb_attr     copyb;         /**< For CopyB operation */
	bound_attr     bound;         /**< For Bound operation */
	conv_attr      conv;          /**< For Conv operation */
	divmod_attr    divmod;        /**< For Div/Mod/DivMod operation */
	asm_attr       assem;         /**< For ASM operation. */
} attr;

/**
 * Edge info to put into an irn.
 */
typedef struct irn_edge_kind_info_t {
	struct list_head outs_head;  /**< The list of all outs. */
	unsigned edges_built : 1;    /**< Set edges where built for this node. */
	unsigned out_count : 31;     /**< Number of outs in the list. */
} irn_edge_info_t;

typedef irn_edge_info_t irn_edges_info_t[EDGE_KIND_LAST];

/**
 * A Def-Use edge.
 */
typedef struct ir_def_use_edge {
	ir_node *use;            /** The use node of that edge. */
	int     pos;             /** The position of this edge in use's input array. */
} ir_def_use_edge;

/**
 * The common structure of an irnode.
 * If the node has some attributes, they are stored in the attr field.
 */
struct ir_node {
	/* ------- Basics of the representation  ------- */
	firm_kind kind;          /**< Distinguishes this node from others. */
	unsigned node_idx;       /**< The node index of this node in its graph. */
	ir_op *op;               /**< The Opcode of this node. */
	ir_mode *mode;           /**< The Mode of this node. */
	struct ir_node **in;     /**< The array of predecessors / operands. */
	ir_visited_t visited;    /**< The visited counter for walks of the graph. */
	void *link;              /**< To attach additional information to the node, e.g.
	                              used during optimization to link to nodes that
	                              shall replace a node. */
	long node_nr;            /**< A globally unique node number for each node. */
	/* ------- Fields for optimizations / analysis information ------- */
	ir_def_use_edge *out;    /**< array of def-use edges. */
	struct dbg_info *dbi;    /**< A pointer to information for debug support. */
	/* ------- For debugging ------- */
#ifdef DEBUG_libfirm
	unsigned out_valid : 1;
	unsigned flags     : 31;
#endif
	/* ------- For analyses -------- */
	ir_loop *loop;           /**< the loop the node is in. Access routines in irloop.h */
	struct ir_node **deps;   /**< Additional dependencies induced by state. */
	void            *backend_info;
	irn_edges_info_t edge_info;  /**< Everlasting out edges. */

	/* ------- Opcode depending fields -------- */
	attr attr;               /**< The set of attributes of this node. Depends on opcode.
	                              Must be last field of struct ir_node. */
};

#include "iredgeset.h"

/**
 * Edge info to put into an irg.
 */
typedef struct irg_edge_info_t {
	ir_edgeset_t     edges;          /**< A set containing all edges of the current graph. */
	struct list_head free_edges;     /**< list of all free edges. */
	struct obstack   edges_obst;     /**< Obstack, where edges are allocated on. */
	unsigned         allocated : 1;  /**< Set if edges are allocated on the obstack. */
	unsigned         activated : 1;  /**< Set if edges are activated for the graph. */
} irg_edge_info_t;

typedef irg_edge_info_t irg_edges_info_t[EDGE_KIND_LAST];

/**
 * Index constants for nodes that can be accessed through the graph anchor node.
 */
enum irg_anchors {
	anchor_end_block = 0,    /**< block the end node will belong to, same as Anchors block */
	anchor_start_block,      /**< block the start node will belong to */
	anchor_end,              /**< end node of this ir_graph */
	anchor_start,            /**< start node of this ir_graph */
	anchor_initial_exec,     /**< methods initial control flow */
	anchor_frame,            /**< methods frame */
	anchor_tls,              /**< pointer to the thread local storage containing all
	                              thread local data. */
	anchor_initial_mem,      /**< initial memory of this graph */
	anchor_args,             /**< methods arguments */
	anchor_bad,              /**< bad node of this ir_graph, the one and
	                              only in this graph */
	anchor_no_mem,           /**< NoMem node of this ir_graph, the one and only in this graph */
	anchor_last
};

/** A callgraph entry for callees. */
typedef struct cg_callee_entry {
	ir_graph  *irg;        /**< The called irg. */
	ir_node  **call_list;  /**< The list of all calls to the irg. */
	int        max_depth;  /**< Maximum depth of all Call nodes to irg. */
} cg_callee_entry;

/**
 * An ir_graph holds all information for a procedure.
 */
struct ir_graph {
	firm_kind         kind;        /**< Always set to k_ir_graph. */
	/* --  Basics of the representation -- */
    unsigned last_node_idx;        /**< The last IR node index for this graph. */
	ir_entity  *ent;               /**< The entity of this procedure, i.e.,
	                                    the type of the procedure and the
	                                    class it belongs to. */
	ir_type *frame_type;           /**< A class type representing the stack frame.
	                                    Can include "inner" methods. */
	ir_node *anchor;               /**< Pointer to the anchor node of this graph. */
	struct obstack *obst;          /**< The obstack where all of the ir_nodes live. */
	ir_node *current_block;        /**< Current block for newly gen_*()-erated ir_nodes. */
	struct obstack *extbb_obst;    /**< The obstack for extended basic block info. */

	/* -- Fields for graph properties -- */
	irg_inline_property inline_property;     /**< How to handle inlineing. */
	unsigned additional_properties;          /**< Additional graph properties. */

	/* -- Fields indicating different states of irgraph -- */
	ir_graph_state_t      state;
	irg_phase_state       phase_state;       /**< Compiler phase. */
	op_pin_state          irg_pinned_state;  /**< Flag for status of nodes. */
	irg_outs_state        outs_state;        /**< Out edges. */
	irg_dom_state         dom_state;         /**< Dominator state information. */
	irg_dom_state         pdom_state;        /**< Post Dominator state information. */
	ir_typeinfo_state     typeinfo_state;    /**< Validity of type information. */
	irg_callee_info_state callee_info_state; /**< Validity of callee information. */
	irg_loopinfo_state    loopinfo_state;    /**< State of loop information. */
	ir_class_cast_state   class_cast_state;  /**< Kind of cast operations in code. */
	irg_extblk_info_state extblk_state;      /**< State of extended basic block info. */
	exec_freq_state       execfreq_state;    /**< Execution frequency state. */
	ir_entity_usage_computed_state entity_usage_state;
	unsigned mem_disambig_opt;               /**< Options for the memory disambiguator. */
	unsigned fp_model;                       /**< floating point model of the graph. */

	/* -- Fields for construction -- */
	int n_loc;                         /**< Number of local variables in this
	                                        procedure including procedure parameters. */
	void **loc_descriptions;           /**< Storage for local variable descriptions. */

	/* -- Fields for optimizations / analysis information -- */
	pset *value_table;                 /**< Hash table for global value numbering (cse)
	                                        for optimizing use in iropt.c */
	ir_def_use_edge *outs;             /**< Space for the Def-Use arrays. */

	ir_loop *loop;                     /**< The outermost loop for this graph. */
	void *link;                        /**< A void* field to link any information to
	                                        the node. */

	ir_graph **callers;                /**< For callgraph analysis: list of caller graphs. */
	unsigned *caller_isbe;             /**< For callgraph analysis: raw bitset if backedge info calculated. */
	cg_callee_entry **callees;         /**< For callgraph analysis: list of callee calls */
	unsigned *callee_isbe;             /**< For callgraph analysis: raw bitset if backedge info calculated. */
	ir_loop   *l;                            /**< For callgraph analysis. */
	int        callgraph_loop_depth;         /**< For callgraph analysis */
	int        callgraph_recursion_depth;    /**< For callgraph analysis */
	double     method_execution_frequency;   /**< For callgraph analysis */


	/* -- Fields for Walking the graph -- */
	ir_visited_t visited;             /**< this flag is an identifier for
	                  ir walk. it will be incremented
	                  every time someone walks through
	                  the graph */
	ir_visited_t block_visited;        /**< same as visited, for a complete block */

	ir_visited_t self_visited;         /**< visited flag of the irg */

	unsigned estimated_node_count;     /**< estimated number of nodes in this graph,
	                                        updated after every walk */
	irg_edges_info_t edge_info;        /**< edge info for automatic outs */
	ir_node **idx_irn_map;             /**< Array mapping node indexes to nodes. */

	int index;                         /**< a unique number for each graph */
	ir_phase *phases[PHASE_LAST+1];    /**< Phase information. */
	void     *be_data;                 /**< backend can put in private data here */

	unsigned  dump_nr;                 /**< number of graph dumps */
#ifdef DEBUG_libfirm
	int   n_outs;                      /**< Size wasted for outs */
	long graph_nr;                     /**< a unique graph number for each
	                                        graph to make output readable. */
#endif

#ifndef NDEBUG
	ir_resources_t reserved_resources; /**< Bitset for tracking used local resources. */
#endif
};

/**
 * Data structure that holds central information about a program
 * or a module.
 * One irp is created by libFirm on construction, so irp should never be NULL.
 *
 * - main_irg:  The ir graph that is the entry point to the program.
 *              (Anything not reachable from here may be optimized away
 *              if this irp represents a whole program.
 * - irg:       List of all ir graphs in the program or module.
 * - type:      A list containing all types known to the translated program.
 *              Some types can have several entries in this list (as a result of
 *              using exchange_types()).
 * - glob_type: The unique global type that is owner of all global entities
 *              of this module.
 */
struct ir_prog {
	firm_kind kind;                 /**< must be k_ir_prog */
	ident     *name;                /**< A file name or the like. */
	ir_graph  *main_irg;            /**< The entry point to the compiled program
	                                     or NULL if no point exists. */
	ir_graph **graphs;              /**< A list of all graphs in the ir. */
	ir_graph  *const_code_irg;      /**< This ir graph gives the proper environment
	                                     to allocate nodes the represent values
	                                     of constant entities. It is not meant as
	                                     a procedure.  */
	ir_type   *segment_types[IR_SEGMENT_LAST+1];
	ir_type  **types;               /**< A list of all types in the ir. */
	ir_mode  **modes;               /**< A list of all modes in the ir. */
	ir_op    **opcodes;             /**< A list of all opcodes in the ir. */
	ident    **global_asms;         /**< An array of global ASM insertions. */

	/* -- states of and access to generated information -- */
	irg_phase_state phase_state;    /**< The state of construction. */

	irg_outs_state outs_state;      /**< The state of out edges of ir nodes. */
	ir_node **ip_outedges;          /**< A huge Array that contains all out edges
	                                     in interprocedural view. */
	irg_outs_state trouts_state;    /**< The state of out edges of type information. */

	irg_callee_info_state callee_info_state; /**< Validity of callee information.
	                                              Contains the lowest value or all irgs.  */
	ir_typeinfo_state typeinfo_state;    /**< Validity of type information. */
	inh_transitive_closure_state inh_trans_closure_state;  /**< State of transitive closure
	                                                            of inheritance relations. */

	irp_callgraph_state callgraph_state; /**< The state of the callgraph. */
	ir_loop *outermost_cg_loop;          /**< For callgraph analysis: entry point
	                                              to looptree over callgraph. */
	int max_callgraph_loop_depth;        /**< needed in callgraph. */
	int max_callgraph_recursion_depth;   /**< needed in callgraph. */
	double max_method_execution_frequency;  /**< needed in callgraph. */
	irp_temperature_state temperature_state; /**< accumulated temperatures computed? */
	exec_freq_state execfreq_state;      /**< The state of execution frequency information */
	loop_nesting_depth_state lnd_state;  /**< The state of loop nesting depth information. */
	ir_class_cast_state class_cast_state;    /**< The state of cast operations in code. */
	ir_entity_usage_computed_state globals_entity_usage_state;

	ir_exc_region_t last_region_nr;      /**< The last exception region number that was assigned. */
	ir_label_t last_label_nr;            /**< The highest label number for generating unique labels. */
	int  max_irg_idx;                    /**< highest unused irg index */
	long max_node_nr;                    /**< to generate unique numbers for nodes. */
	unsigned dump_nr;                    /**< number of program info dumps */
#ifndef NDEBUG
	ir_resources_t reserved_resources;   /**< Bitset for tracking used global resources. */
#endif
};

#endif
