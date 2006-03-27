#ifndef _MIPS_NODES_ATTR_H_
#define _MIPS_NODES_ATTR_H_

#include "../bearch.h"
#include "irmode_t.h"

typedef struct _mips_register_req_t {
	const arch_register_req_t req;
	int same_pos;        /**<< in case of "should be same" we need to remember the pos to get the irn */
	int different_pos;   /**<< in case of "should be different" we need to remember the pos to get the irn */
} mips_register_req_t;


typedef struct _mips_attr_t {
	arch_irn_flags_t flags;     /**<< indicating if spillable, rematerializeable ... etc. */
	int              n_res;     /**<< number of results for this node */

	tarval *tv;					/**<< contains the immediate value (if the node has any) */
	ident *symconst_id;			/**<< contains the ident (for la operations) */

	union {
		ir_mode *load_store_mode;	/**<< contains the mode of a load/store */
		ir_mode *original_mode;		/**<< contains the original mode of the node */
	} modes;
	entity *stack_entity;		/**<< contains the entity on the stack for a load/store mode */
	int stack_entity_offset;	/**<< contains the real stack offset for the entity */
	int switch_default_pn;		/**< proj number of default case in switch */

	const mips_register_req_t **in_req;  /**<< register requirements for arguments */
	const mips_register_req_t **out_req; /**<< register requirements for results */

	const arch_register_t **slots;       /**<< register slots for assigned registers */
} mips_attr_t;

#endif /* _mips_NODES_ATTR_H_ */
