/* Copyright (C) 1998 - 2000 by Universitaet Karlsruhe
 * All rights reserved.
 *
 * Authors: Christian Schaefer
 *
 *
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

# include "irgraph_t.h"
# include "irvrfy.h"
# include "irgwalk.h"

#ifdef NDEBUG
#define ASSERT_AND_RET(expr, string, ret)  if (!(expr)) return (ret)
#else
#define ASSERT_AND_RET(expr, string, ret)  do { assert((expr) && string); if (!(expr)) return (ret); } while(0)
#endif

/* @@@ replace use of array "in" by access functions. */
ir_node **get_irn_in(ir_node *node);

INLINE static int
vrfy_Proj_proj(ir_node *p, ir_graph *irg) {
  ir_node *pred;
  ir_mode *mode;
  int proj;

  pred = skip_nop(get_Proj_pred(p));
  assert(get_irn_mode(pred) == mode_T);
  mode = get_irn_mode(p);
  proj = get_Proj_proj(p);

  switch (get_irn_opcode(pred)) {
    case iro_Start:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_X) ||
           (proj == 1 && mode == mode_M) ||
           (proj == 2 && mode == mode_P) ||
           (proj == 3 && mode == mode_P) ||
           (proj == 4 && mode == mode_T)),
          "wrong Proj from Start", 0);
      break;

    case iro_Cond:
      ASSERT_AND_RET( (proj >= 0 && mode == mode_X), "wrong Proj from Cond", 0);
      break;

    case iro_Raise:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_X) ||
           (proj == 1 && mode == mode_M)),
          "wrong Proj from Raise", 0);
      break;

    case iro_InstOf:
      ASSERT_AND_RET( (proj >= 0 && mode == mode_X), "wrong Proj from InstOf", 0);
      break;

    case iro_Call:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 && mode == mode_X) ||
           (proj == 2 && mode == mode_T) ||
           (proj == 3 && mode == mode_M)),
          "wrong Proj from Call", 0);
      break;

    case iro_Quot:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 && mode == mode_X) ||
           (proj == 2 && mode_is_float(mode))),
          "wrong Proj from Quot", 0);
      break;

    case iro_DivMod:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 && mode == mode_X) ||
           (proj == 2 && mode == mode_Is) ||
           (proj == 3 && mode_is_int(mode))),
          "wrong Proj from DivMod", 0);
      break;

    case iro_Div:
    case iro_Mod:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 && mode == mode_X) ||
           (proj == 2 && mode_is_int(mode))),
          "wrong Proj from Div or Mod", 0);
      break;

    case iro_Cmp:
      ASSERT_AND_RET(
          (proj >= 0 && proj <= 15 && mode == mode_b),
          "wrong Proj from Cmp", 0);
      break;

    case iro_Load:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 && mode == mode_X) ||
           (proj == 2 && mode_is_data(mode))),
          "wrong Proj from Load", 0);
      break;

    case iro_Store:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 && mode == mode_X)),
          "wrong Proj from Store", 0);
      break;

    case iro_Alloc:
      ASSERT_AND_RET(
          ((proj == 0 && mode == mode_M) ||
           (proj == 1 /* && mode == mode_X*/) ||
           (proj == 2 && mode == mode_P)),
          "wrong Proj from Alloc", 0);
      break;

    case iro_Proj:
      {
        type *mt; /* A method type */
        pred = skip_nop(get_Proj_pred(pred));
        ASSERT_AND_RET((get_irn_mode(pred) == mode_T), "Proj from something not a tuple", 0);
        switch (get_irn_opcode(pred))
        {
          case iro_Start:
            {
              ASSERT_AND_RET(
                  (proj >= 0 && mode_is_data(mode)),
                  "wrong Proj from Proj from Start", 0);
              mt = get_entity_type(get_irg_ent(irg));
              ASSERT_AND_RET(
                  (proj < get_method_n_params(mt)),
                  "More Projs for args than args in type", 0);
              if ((mode == mode_P) && is_compound_type(get_method_param_type(mt, proj)))
                /* value argument */ break;

              ASSERT_AND_RET(
                  (mode == get_type_mode(get_method_param_type(mt, proj))),
                  "Mode of Proj from Start doesn't match mode of param type.", 0);
            }
            break;

          case iro_Call:
            {
              ASSERT_AND_RET(
                  (proj >= 0 && mode_is_data(mode)),
                  "wrong Proj from Proj from Call", 0);
              mt = get_Call_type(pred);
              ASSERT_AND_RET(
                  (proj < get_method_n_ress(mt)),
                  "More Projs for results than results in type.", 0);
              if ((mode == mode_P) && is_compound_type(get_method_res_type(mt, proj)))
                /* value result */ break;

              ASSERT_AND_RET(
                  (mode == get_type_mode(get_method_res_type(mt, proj))),
                  "Mode of Proj from Call doesn't match mode of result type.", 0);
            }
            break;

          case iro_Tuple:
            /* We don't test */
            break;

          default:
            ASSERT_AND_RET(0, "Unknown opcode", 0);
        }
        break;

      }
    case iro_Tuple:
      /* We don't test */
      break;

    case iro_CallBegin:
      break;

    case iro_EndReg:
      break;

    case iro_EndExcept:
      break;

    default:
      ASSERT_AND_RET(0, "Unknown opcode", 0);
  }

  /* all went ok */
  return 1;
}

int irn_vrfy_irg(ir_node *n, ir_graph *irg)
{
  int i;
  int opcode, opcode1;
  ir_mode *mymode, *op1mode = NULL, *op2mode, *op3mode;
  int op_is_symmetric = 1;  /*  0: asymmetric
1: operands have identical modes
2: modes of operands == mode of this node */
  type *mt; /* A method type */

  ir_node **in;

  if (! interprocedural_view) {
    /*
     * do NOT check placement in interprocedural view, as we don't always know
     * the "right" graph ...
     */
    ASSERT_AND_RET(node_is_in_irgs_storage(irg, n), "Node is not stored on proper IR graph!", 0);
  }

  opcode = get_irn_opcode (n);

  /* We don't want to test nodes whose predecessors are Bad or Unknown,
     as we would have to special case that for each operation. */
  if (opcode != iro_Phi && opcode != iro_Block)
    for (i = 0; i < get_irn_arity(n); i++) {
      opcode1 = get_irn_opcode(get_irn_n(n, i));
      if (opcode1 == iro_Bad || opcode1 == iro_Unknown)
        return 1;
    }

  mymode = get_irn_mode (n);
  in = get_irn_in (n);

  switch (opcode)
  {
    case iro_Start:
      ASSERT_AND_RET(
          /* Start: BB --> X x M x P x data1 x ... x datan */
          mymode == mode_T, "Start node", 0
          );
      break;

    case iro_Jmp:
      ASSERT_AND_RET(
          /* Jmp: BB --> X */
          mymode == mode_X, "Jmp node", 0
          );
      break;

    case iro_Break:
      ASSERT_AND_RET(
          /* Jmp: BB --> X */
          mymode == mode_X, "Jmp node", 0
          );
      break;

    case iro_Cond:
      op1mode = get_irn_mode(in[1]);
      ASSERT_AND_RET(
          /* Cond: BB x b --> X x X */
          (op1mode == mode_b ||
           /* Cond: BB x int --> X^n */
           mode_is_int(op1mode) ),  "Cond node", 0
          );
      ASSERT_AND_RET(mymode == mode_T, "Cond mode is not a tuple", 0);
      break;

    case iro_Return:
      op1mode = get_irn_mode(in[1]);
      /* Return: BB x M x data1 x ... x datan --> X */
      /* printf("mode: %s, code %s\n", ID_TO_STR(n->mode->name), ID_TO_STR(n->op->name));*/
      ASSERT_AND_RET( op1mode == mode_M, "Return node", 0 );  /* operand M */
      for (i=2; i < get_irn_arity(n); i++) {
        ASSERT_AND_RET( mode_is_data(get_irn_mode(in[i])), "Return node", 0 );  /* operand datai */
      };
      ASSERT_AND_RET( mymode == mode_X, "Result X", 0 );   /* result X */
      /* Compare returned results with result types of method type */
      mt = get_entity_type(get_irg_ent(irg));
      ASSERT_AND_RET( get_Return_n_ress(n) == get_method_n_ress(mt),
          "Number of results for Return doesn't match number of results in type.", 0 );
      for (i = 0; i < get_Return_n_ress(n); i++)
        ASSERT_AND_RET(
            get_irn_mode(get_Return_res(n, i)) == get_type_mode(get_method_res_type(mt, i)),
            "Mode of result for Return doesn't match mode of result type.", 0);
      break;

    case iro_Raise:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Sel: BB x M x P --> X x M */
          op1mode == mode_M && op2mode == mode_P &&
          mymode == mode_T, "Raise node", 0
          );
      break;

    case iro_Const:
      ASSERT_AND_RET(
          /* Const: BB --> data */
          (mode_is_data (mymode) ||
           mymode == mode_b)      /* we want boolean constants for static evaluation */
          ,"Const node", 0        /* of Cmp. */
          );
      break;

    case iro_SymConst:
      ASSERT_AND_RET(
          /* SymConst: BB --> int*/
          (mode_is_int(mymode) ||
           /* SymConst: BB --> P*/
           mymode == mode_P)
          ,"SymConst node", 0);
      break;

    case iro_Sel:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Sel: BB x M x P x int^n --> P */
          op1mode == mode_M && op2mode == mode_P &&
          mymode == mode_P, "Sel node", 0
          );
      for (i=3; i < get_irn_arity(n); i++)
      {
        ASSERT_AND_RET(mode_is_int(get_irn_mode(in[i])), "Sel node", 0);
      }
      break;

    case iro_InstOf:
      ASSERT_AND_RET(mode_T == mymode, "mode of Instof is not a tuple", 0);
      ASSERT_AND_RET(mode_is_data(op1mode), "Instof not on data", 0);
      break;

    case iro_Call:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      /* Call: BB x M x P x data1 x ... x datan
         --> M x datan+1 x ... x data n+m */
      ASSERT_AND_RET( op1mode == mode_M && op2mode == mode_P, "Call node", 0 );  /* operand M x P */
      for (i=3; i < get_irn_arity(n); i++) {
        ASSERT_AND_RET( mode_is_data(get_irn_mode(in[i])), "Call node", 0 );  /* operand datai */
      };
      ASSERT_AND_RET( mymode == mode_T, "Call result not a tuple", 0 );   /* result T */
      /* Compare arguments of node with those of type */
      mt = get_Call_type(n);

      if (get_method_variadicity(mt) == variadic) {
        ASSERT_AND_RET(
            get_Call_n_params(n) >= get_method_n_params(mt),
            "Number of args for Call doesn't match number of args in variadic type.",
            0);
      }
      else {
        ASSERT_AND_RET(
            get_Call_n_params(n) == get_method_n_params(mt),
            "Number of args for Call doesn't match number of args in non variadic type.",
            0);
      }

      for (i = 0; i < get_method_n_params(mt); i++) {
        ASSERT_AND_RET(
            get_irn_mode(get_Call_param(n, i)) == get_type_mode(get_method_param_type(mt, i)),
            "Mode of arg for Call doesn't match mode of arg type.", 0);
      }
      break;

    case iro_Add:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* common Add: BB x num x num --> num */
          (( op1mode == mymode && op2mode == op1mode && mode_is_num(mymode)) ||
           /* Pointer Add: BB x P x int --> P */
           (op1mode == mode_P && mode_is_int(op2mode) && mymode == mode_P) ||
           /* Pointer Add: BB x int x P --> P */
           (mode_is_int(op1mode) && op2mode == mode_P && mymode == mode_P)),
          "Add node", 0
          );
      if (op1mode == mode_P || op2mode == mode_P) {
        /* BB x P x int --> P or BB x int x P --> P */
        op_is_symmetric = 0; /* ArmRoq */
      } else {
        /* BB x num x num --> num */
        op_is_symmetric = 2;
      }
      break;

    case iro_Sub:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* common Sub: BB x num x num --> num */
          ((mymode ==op1mode && mymode == op2mode && mode_is_num(op1mode)) ||
           /* Pointer Sub: BB x P x int --> P */
           (op1mode == mode_P && mode_is_int(op2mode) && mymode == mode_P) ||
           /* Pointer Sub: BB x int x P --> P */
           (mode_is_int(op1mode) && op2mode == mode_P && mymode == mode_P) ||
           /* Pointer Sub: BB x P x P --> int */
           (op1mode == mode_P && op2mode == mode_P && mode_is_int(mymode))),
          "Sub node", 0
          );
      if (op1mode == mode_P && op2mode == mode_P) {
        op_is_symmetric = 1; /* ArmRoq */
      } else if (op1mode == mode_P || op2mode == mode_P) {
        op_is_symmetric = 0; /* ArmRoq */
      } else {
        op_is_symmetric = 2;
      }
      break;

    case iro_Minus:
      op1mode = get_irn_mode(in[1]);
      ASSERT_AND_RET(
          /* Minus: BB x float --> float */
          op1mode == mymode && get_mode_sort(op1mode) == irms_float_number, "Minus node", 0
          );
      op_is_symmetric = 2;
      break;

    case iro_Mul:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Mul: BB x int1 x int1 --> int2 */
          mode_is_int(op1mode) &&
          op2mode == op1mode &&
          mode_is_int(mymode),
          "Mul node",0
          );
      op_is_symmetric = 2;
      break;

    case iro_Quot:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      op3mode = get_irn_mode(in[3]);
      ASSERT_AND_RET(
          /* Quot: BB x M x float x float --> M x X x float */
          op1mode == mode_M && op2mode == op3mode &&
          get_mode_sort(op2mode) == irms_float_number &&
          mymode == mode_T,
          "Quot node",0
          );
      op_is_symmetric = 2;
      break;

    case iro_DivMod:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      op3mode = get_irn_mode(in[3]);
      ASSERT_AND_RET(
          /* DivMod: BB x M x int x int --> M x X x int x int */
          op1mode == mode_M &&
          mode_is_int(op2mode) &&
          op3mode == op2mode &&
          mymode == mode_T,
          "DivMod node", 0
          );
      op_is_symmetric = 1;
      break;

    case iro_Div:
    case iro_Mod:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      op3mode = get_irn_mode(in[3]);
      ASSERT_AND_RET(
          /* Div or Mod: BB x M x int x int --> M x X x int */
          op1mode == mode_M &&
          op2mode == op3mode &&
          mode_is_int(op2mode) &&
          mymode == mode_T,
          "Div or Mod node", 0
          );
      op_is_symmetric = 1;
      break;

    case iro_Abs:
      op1mode = get_irn_mode(in[1]);
      ASSERT_AND_RET(
          /* Abs: BB x num --> num */
          op1mode == mymode &&
          mode_is_num (op1mode),
          "Abs node",0
          );
      op_is_symmetric = 2;
      break;

    case iro_And:
    case iro_Or:
    case iro_Eor:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* And or Or or Eor: BB x int x int --> int */
          mode_is_int(mymode) &&
          op2mode == op1mode &&
          mymode == op2mode,
          "And, Or or Eor node", 0
          );
      op_is_symmetric = 2;
      break;

    case iro_Not:
      op1mode = get_irn_mode(in[1]);
      ASSERT_AND_RET(
          /* Not: BB x int --> int */
          mode_is_int(mymode) &&
          mymode == op1mode,
          "Not node", 0
          );
      op_is_symmetric = 2;
      break;


    case iro_Cmp:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Cmp: BB x datab x datab --> b16 */
          mode_is_data (op1mode) &&
          op2mode == op1mode &&
          mymode == mode_T,
          "Cmp node", 0
          );
      break;

    case iro_Shl:
    case iro_Shr:
    case iro_Shrs:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      assert(
          /* Shl, Shr or Shrs: BB x int x int_u --> int */
          mode_is_int(op1mode) &&
          mode_is_int(op2mode) &&
          !mode_is_signed(op2mode) &&
          mymode == op1mode &&
          "Shl, Shr, Shr or Rot node"
          );
      break;

    case iro_Rot:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Rot: BB x int x int --> int */
          mode_is_int(op1mode) &&
          mode_is_int(op2mode) &&
          mymode == op1mode,
          "Rot node",0
          );
      break;

    case iro_Conv:
      op1mode = get_irn_mode(in[1]);
      ASSERT_AND_RET(
          /* Conv: BB x datab1 --> datab2 */
          mode_is_datab(op1mode) && mode_is_data(mymode),
          "Conv node", 0
          );
      break;

    case iro_Phi:
      /* Phi: BB x dataM^n --> dataM */
      /* for some reason "<=" aborts. int there a problem with get_store? */
      for (i=1; i < get_irn_arity(n); i++) {
        if (!is_Bad(in[i]) && (get_irn_op(in[i]) != op_Unknown))
          ASSERT_AND_RET( get_irn_mode(in[i]) == mymode, "Phi node", 0);
      };
      ASSERT_AND_RET( mode_is_dataM(mymode), "Phi node", 0 );
      break;

    case iro_Load:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Load: BB x M x P --> M x X x data */
          op1mode == mode_M && op2mode == mode_P,
          "Load node", 0
          );
      ASSERT_AND_RET( mymode == mode_T, "Load node", 0 );
      break;

    case iro_Store:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      op3mode = get_irn_mode(in[3]);
      ASSERT_AND_RET(
          /* Load: BB x M x P x data --> M x X */
          op1mode == mode_M && op2mode == mode_P && mode_is_data(op3mode),
          "Store node", 0
          );
      ASSERT_AND_RET(mymode == mode_T, "Store node", 0);
      break;

    case iro_Alloc:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Alloc: BB x M x int_u --> M x X x P */
          op1mode == mode_M &&
          mode_is_int(op2mode) &&
          !mode_is_signed(op2mode) &&
          mymode == mode_T,
          "Alloc node", 0
          );
      break;

    case iro_Free:
      op1mode = get_irn_mode(in[1]);
      op2mode = get_irn_mode(in[2]);
      ASSERT_AND_RET(
          /* Free: BB x M x P --> M */
          op1mode == mode_M && op2mode == mode_P &&
          mymode == mode_M,
          "Free node",0
          );
      break;

    case iro_Sync:
      /* Sync: BB x M^n --> M */
      for (i=1; i < get_irn_arity(n); i++) {
        ASSERT_AND_RET( get_irn_mode(in[i]) == mode_M, "Sync node", 0 );
      };
      ASSERT_AND_RET( mymode == mode_M, "Sync node", 0 );
      break;

    case iro_Proj:
      return vrfy_Proj_proj(n, irg);
      break;

    default:
      break;
  }

  /* All went ok */
  return 1;
}

int irn_vrfy(ir_node *n)
{
  return irn_vrfy_irg(n, current_ir_graph);
}

/*******************************************************************/
/* Verify the whole graph.                                         */
/*******************************************************************/

static void vrfy_wrap(ir_node *node, void *env)
{
  int *res = env;

  *res = irn_vrfy(node);
}

int irg_vrfy(ir_graph *irg)
{
  int res = 1;
  ir_graph *rem;

  rem = current_ir_graph;
  current_ir_graph = irg;

  assert(get_irg_pinned(irg) == pinned);

  irg_walk(irg->end, vrfy_wrap, NULL, &res);

  current_ir_graph = rem;

  return res;
}
