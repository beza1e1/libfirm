/* -------------------------------------------------------------------
 * $Id$
 * -------------------------------------------------------------------
 * Intraprozedurale Analyse zur Absch�tzung der Aufrulrelation. Es
 * wird eine Menge von freien Methoden und anschlie�end die an den
 * Call-Operationen aufrufbaren Methoden bestimmt.
 *
 * Erstellt: Hubert Schmid, 09.06.2002
 * ---------------------------------------------------------------- */


#include "cgana.h"


#include "eset.h"
#include "pmap.h"
#include "array.h"
#include "irprog.h"
#include "irgwalk.h"
#include "ircons.h"
#include "irgmod.h"


/* Eindeutige Adresse zur Markierung von besuchten Knoten und zur
 * Darstellung der unbekannten Methode. */
static void * MARK = &MARK;



/* --- sel methods ---------------------------------------------------------- */


static eset * entities = NULL;


/* Bestimmt die eindeutige Methode, die die Methode f�r den
 * �bergebenene (dynamischen) Typ �berschreibt. */
static entity * get_implementation(type * class, entity * method) {
  int i;
  if (get_entity_peculiarity(method) != description && get_entity_owner(method) == class) {
    return method;
  }
  for (i = get_entity_n_overwrittenby(method) - 1; i >= 0; --i) {
    entity * e = get_entity_overwrittenby(method, i);
    if (get_entity_peculiarity(e) != description && get_entity_owner(e) == class) {
      return e;
    }
  }
  for (i = get_class_n_supertype(class) - 1; i >= 0; --i) {
    entity * e = get_implementation(get_class_supertype(class, i), method);
    if (e) {
      return e;
    }
  }
  assert(0 && "implemenation not found");
}


/* Alle Methoden bestimmen, die die �bergebene Methode �berschreiben
 * (und implementieren). In der zur�ckgegebenen Reihung kommt jede
 * Methode nur einmal vor. Der Wert 'NULL' steht f�r unbekannte
 * (externe) Methoden. Die zur�ckgegebene Reihung mu� vom Aufrufer
 * wieder freigegeben werden (siehe "DEL_ARR_F"). Gibt es �berhaupt
 * keine Methoden, die die "method" �berschreiben, so gibt die Methode
 * "NULL" zur�ck. */
static entity ** get_impl_methods(entity * method) {
  eset * set = eset_create();
  int size = 0;
  int i;
  entity ** arr;
  bool open = false;
  if (get_entity_peculiarity(method) == existent) {
    if (get_entity_visibility(method) == external_allocated) {
      assert(get_entity_irg(method) == NULL);
      open = true;
    } else {
      assert(get_entity_irg(method) != NULL);
      eset_insert(set, method);
      ++size;
    }
  }
  for (i = get_entity_n_overwrittenby(method) - 1; i >= 0; --i) {
    entity * ent = get_entity_overwrittenby(method, i);
    if (get_entity_peculiarity(ent) == existent) {
      if (get_entity_visibility(ent) == external_allocated) {
	assert(get_entity_irg(ent) == NULL);
	open = true;
      } else {
	assert(get_entity_irg(ent) != NULL);
	if (!eset_contains(set, ent)) {
	  eset_insert(set, ent);
	  ++size;
	}
      }
    }
  }
  if (size == 0 && !open) {
    /* keine implementierte �berschriebene Methode */
    arr = NULL;
  } else if (open) {
    entity * ent;
    arr = NEW_ARR_F(entity *, size + 1);
    arr[0] = NULL;
    for (ent = eset_first(set); size > 0; ent = eset_next(set), --size) arr[size] = ent;
  } else {
    entity * ent;
    arr = NEW_ARR_F(entity *, size);
    for (size -= 1, ent = eset_first(set); size >= 0; ent = eset_next(set), --size) arr[size] = ent;
  }
  eset_destroy(set);
  return arr;
}


static void sel_methods_walker(ir_node * node, pmap * ldname_map) {
  if (get_irn_op(node) == op_SymConst) {
    /* Wenn m�glich SymConst-Operation durch Const-Operation
     * ersetzen. */
    if (get_SymConst_kind(node) == linkage_ptr_info) {
      pmap_entry * entry = pmap_find(ldname_map, (void *) get_SymConst_ptrinfo(node));
      if (entry != NULL) {
	entity * ent = entry->value;
	if (get_entity_visibility(ent) != external_allocated) {
	  assert(get_entity_irg(ent));
	  set_irg_current_block(current_ir_graph, get_nodes_Block(node));
	  exchange(node, new_Const(mode_p, tarval_p_from_entity(ent)));
	}
      }
    }
  } else if (get_irn_op(node) == op_Sel && is_method_type(get_entity_type(get_Sel_entity(node)))) {
    entity * ent = get_Sel_entity(node);
    if (get_irn_op(skip_Proj(get_Sel_ptr(node))) == op_Alloc) {
      ent = get_implementation(get_Alloc_type(skip_Proj(get_Sel_ptr(node))), ent);
      if (get_entity_visibility(ent) == external_allocated) {
	exchange(node, new_SymConst((type_or_id_p) get_entity_ld_ident(ent), linkage_ptr_info));
      } else {
	exchange(node, new_Const(mode_p, tarval_p_from_entity(ent)));
      }
    } else {
      if (!eset_contains(entities, ent)) {
	/* Entity noch nicht behandelt. Alle (intern oder extern)
	 * implementierten Methoden suchen, die diese Entity
	 * �berschreiben. Die Menge an entity.link speichern. */
	set_entity_link(ent, get_impl_methods(ent));
	eset_insert(entities, ent);
      }
      if (get_entity_link(ent) == NULL) {
	/* Die Sel-Operation kann nie einen Zeiger auf eine aufrufbare
	 * Methode zur�ckgeben. Damit ist sie insbesondere nicht
	 * ausf�hrbar und nicht erreichbar. */
	/* Gib eine Warnung aus wenn die Entitaet eine Beschreibung ist
	   fuer die es keine Implementierung gibt. */
	if (get_entity_peculiarity(ent) == description) {
	  /* @@@ GL Methode um Fehler anzuzeigen aufrufen! */
	  xprintf("WARNING: Calling method description %I in method %I which has "
		  "no implementation!\n", get_entity_ident(ent),
		  get_entity_ident(get_irg_ent(current_ir_graph)));
	} else {
	  exchange(node, new_Bad());
	}
      } else {
	entity ** arr = get_entity_link(ent);
	if (ARR_LEN(arr) == 1 && arr[0] != NULL) {
	  /* Die Sel-Operation kann immer nur einen Wert auf eine
	   * interne Methode zur�ckgeben. Wir k�nnen daher die
	   * Sel-Operation durch eine Const- bzw. SymConst-Operation
	   * ersetzen. */
	  if (get_entity_visibility(arr[0]) == external_allocated) {
	    exchange(node, new_SymConst((type_or_id_p) get_entity_ld_ident(arr[0]),
					linkage_ptr_info));
	  } else {
	    exchange(node, new_Const(mode_p, tarval_p_from_entity(arr[0])));
	  }
	}
      }
    }
  }
}


/* Datenstruktur initialisieren. Zus�tzlich werden alle
 * SymConst-Operationen, die auf interne Methode verweisen, durch
 * Const-Operationen ersetzt. */
static void sel_methods_init(void) {
  int i;
  pmap * ldname_map = pmap_create();
  assert(entities == NULL);
  entities = eset_create();
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    entity * ent = get_irg_ent(get_irp_irg(i));
    /* Nur extern sichtbare Methode k�nnen �berhaupt mit SymConst
     * aufgerufen werden. */
    if (get_entity_visibility(ent) != local) {
      pmap_insert(ldname_map, (void *) get_entity_ld_ident(ent), ent);
    }
  }
  all_irg_walk((irg_walk_func) sel_methods_walker, NULL, ldname_map);
  pmap_destroy(ldname_map);
}


/* Datenstruktur freigeben. */
static void sel_methods_dispose(void) {
  entity * ent;
  assert(entities);
  for (ent = eset_first(entities); ent; ent = eset_next(entities)) {
    entity ** arr = get_entity_link(ent);
    if (arr) {
      DEL_ARR_F(arr);
    }
    set_entity_link(ent, NULL);
  }
  eset_destroy(entities);
  entities = NULL;
}


/* Gibt die Menge aller Methoden zur�ck, die an diesem Sel-Knoten
 * zur�ckgegeben werden k�nnen. Die Liste enth�lt keine doppelten
 * Eintr�ge. */
static entity ** get_Sel_arr(ir_node * sel) {
  static entity ** NULL_ARRAY = NULL;
  entity * ent;
  entity ** arr;
  assert(sel && get_irn_op(sel) == op_Sel);
  ent = get_Sel_entity(sel);
  assert(is_method_type(get_entity_type(ent))); /* what else? */
  arr = get_entity_link(ent);
  if (arr) {
    return arr;
  } else {
    /* "NULL" zeigt an, dass keine Implementierung existiert. Dies
     * kann f�r polymorphe (abstrakte) Methoden passieren. */
    if (!NULL_ARRAY) {
      NULL_ARRAY = NEW_ARR_F(entity *, 0);
    }
    return NULL_ARRAY;
  }
}


static int get_Sel_n_methods(ir_node * sel) {
  return ARR_LEN(get_Sel_arr(sel));
}


static entity * get_Sel_method(ir_node * sel, int pos) {
  entity ** arr = get_Sel_arr(sel);
  assert(pos >= 0 && pos < ARR_LEN(arr));
  return arr[pos];
}



/* --- callee analysis ------------------------------------------------------ */


static void callee_ana_node(ir_node * node, eset * methods);


static void callee_ana_proj(ir_node * node, long n, eset * methods) {
  assert(get_irn_mode(node) == mode_T);
  if (get_irn_link(node) == MARK) {
    /* already visited */
    return;
  }
  set_irn_link(node, MARK);

  switch (get_irn_opcode(node)) {
  case iro_Proj: {
    /* proj_proj: in einem "sinnvollen" Graphen kommt jetzt ein
     * op_Tuple oder ein Knoten, der eine "freie Methode"
     * zur�ckgibt. */
    ir_node * pred = get_Proj_pred(node);
    if (get_irn_link(pred) != MARK) {
      if (get_irn_op(pred) == op_Tuple) {
	callee_ana_proj(get_Tuple_pred(pred, get_Proj_proj(node)), n, methods);
      } else {
	eset_insert(methods, MARK); /* free method -> unknown */
      }
    }
    break;
  }

  case iro_Tuple:
    callee_ana_node(get_Tuple_pred(node, n), methods);
    break;

  case iro_Id:
    callee_ana_proj(get_Id_pred(node), n, methods);
    break;

  default:
    eset_insert(methods, MARK); /* free method -> unknown */
    break;
  }

  set_irn_link(node, NULL);
}


static void callee_ana_node(ir_node * node, eset * methods) {
  int i;

  assert(get_irn_mode(node) == mode_p);
  /* rekursion verhindern */
  if (get_irn_link(node) == MARK) {
    /* already visited */
    return;
  }
  set_irn_link(node, MARK);

  switch (get_irn_opcode(node)) {
  case iro_SymConst:
    /* externe Methode (wegen fix_symconst!) */
    eset_insert(methods, MARK); /* free method -> unknown */
    break;

  case iro_Const: {
    /* interne Methode */
    entity * ent = get_Const_tarval(node)->u.p.ent;
    assert(ent && is_method_type(get_entity_type(ent)));
    if (get_entity_visibility(ent) != external_allocated) {
      assert(get_entity_irg(ent));
      eset_insert(methods, ent);
    } else {
      eset_insert(methods, MARK); /* free method -> unknown */
    }
    break;
  }

  case iro_Sel:
    /* polymorphe Methode */
    for (i = get_Sel_n_methods(node) - 1; i >= 0; --i) {
      entity * ent = get_Sel_method(node, i);
      if (ent) {
	eset_insert(methods, ent);
      } else {
	eset_insert(methods, MARK);
      }
    }
    break;

  case iro_Bad:
    /* nothing */
    break;

  case iro_Phi: /* Vereinigung */
    for (i = get_Phi_n_preds(node) - 1; i >= 0; --i) {
      callee_ana_node(get_Phi_pred(node, i), methods);
    }
    break;

  case iro_Id:
    callee_ana_node(get_Id_pred(node), methods);
    break;

  case iro_Proj:
    callee_ana_proj(get_Proj_pred(node), get_Proj_proj(node), methods);
    break;

  case iro_Add:
  case iro_Sub:
  case iro_Conv:
    /* extern */
    eset_insert(methods, MARK); /* free method -> unknown */
    break;

  default:
    assert(0 && "invalid opcode or opcode not implemented");
    break;
  }

  set_irn_link(node, NULL);
}


static void callee_walker(ir_node * call, void * env) {
  if (get_irn_op(call) == op_Call) {
    eset * methods = eset_create();
    entity * ent;
    entity ** arr = NEW_ARR_F(entity *, 0);
    callee_ana_node(skip_nop(get_Call_ptr(call)), methods);
    if (eset_contains(methods, MARK)) { /* unknown method */
      ARR_APP1(entity *, arr, NULL);
    }
    for (ent = eset_first(methods); ent; ent = eset_next(methods)) {
      if (ent != MARK) {
	ARR_APP1(entity *, arr, ent);
      }
    }
    if (ARR_LEN(arr) == 0) {
      /* Kann vorkommen, wenn der Vorg�nger beispielsweise eine
       * Sel-Operation war, die keine Methoden zur�ckgeben
       * konnte. Wir ersetzen die Call-Operation ebenfalls durch
       * eine Bad-Operation. Die Verlinkung muss wiederhergestellt
       * werden! */
      exchange(call, new_Bad());
    } else {
      set_Call_callee_arr(call, ARR_LEN(arr), arr);
    }
    DEL_ARR_F(arr);
    eset_destroy(methods);
  }
}


/* Bestimmt f�r jede Call-Operation die Menge der aufrufbaren Methode
 * und speichert das Ergebnis in der Call-Operation. (siehe
 * "set_Call_callee"). "sel_methods" wird f�r den Aufbau ben�tigt und
 * muss bereits aufgebaut sein. */
static void callee_ana(void) {
  int i;
  /* Alle Graphen analysieren. */
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    irg_walk_graph(get_irp_irg(i), callee_walker, NULL, NULL);
  }
}



/* --- free method analysis ------------------------------------------------- */


static void free_mark(ir_node * node, eset * set);

static void free_mark_proj(ir_node * node, long n, eset * set) {
  assert(get_irn_mode(node) == mode_T);
  if (get_irn_link(node) == MARK) {
    /* already visited */
    return;
  }
  set_irn_link(node, MARK);
  switch (get_irn_opcode(node)) {
  case iro_Proj: {
    /* proj_proj: in einem "sinnvollen" Graphen kommt jetzt ein
     * op_Tuple oder ein Knoten, der in "free_ana_walker" behandelt
     * wird. */
    ir_node * pred = get_Proj_pred(node);
    if (get_irn_link(pred) != MARK && get_irn_op(pred) == op_Tuple) {
      free_mark_proj(get_Tuple_pred(pred, get_Proj_proj(node)), n, set);
    } else {
      /* nothing: da in "free_ana_walker" behandelt. */
    }
    break;
  }

  case iro_Tuple:
    free_mark(get_Tuple_pred(node, n), set);
    break;

  case iro_Id:
    free_mark_proj(get_Id_pred(node), n, set);
    break;

  case iro_Start:
  case iro_Alloc:
  case iro_Load:
    /* nothing: Die Operationen werden in "free_ana_walker" selbst
     * behandelt. */
    break;

  default:
    assert(0 && "unexpected opcode or opcode not implemented");
    break;
  }
  set_irn_link(node, NULL);
}


static void free_mark(ir_node * node, eset * set) {
  int i;
  assert(get_irn_mode(node) == mode_p);
  if (get_irn_link(node) == MARK) {
    return; /* already visited */
  }
  set_irn_link(node, MARK);
  switch (get_irn_opcode(node)) {
  case iro_Sel: {
    entity * ent = get_Sel_entity(node);
    if (is_method_type(get_entity_type(ent))) {
      for (i = get_Sel_n_methods(node) - 1; i >= 0; --i) {
	eset_insert(set, get_Sel_method(node, i));
      }
    }
    break;
  }
  case iro_SymConst:
    /* nothing: SymConst points to extern method */
    break;
  case iro_Const: {
    tarval * val = get_Const_tarval(node);
    entity * ent = val->u.p.ent;
    if (ent != NULL && is_method_type(get_entity_type(ent))) {
      eset_insert(set, ent);
    }
    break;
  }
  case iro_Phi:
    for (i = get_Phi_n_preds(node) - 1; i >= 0; --i) {
      free_mark(get_Phi_pred(node, i), set);
    }
    break;
  case iro_Id:
    free_mark(get_Id_pred(node), set);
    break;
  case iro_Proj:
    free_mark_proj(get_Proj_pred(node), get_Proj_proj(node), set);
    break;
  default:
    /* nothing: Wird unten behandelt! */
    break;
  }
  set_irn_link(node, NULL);
}


static void free_ana_walker(ir_node * node, eset * set) {
  int i;
  if (get_irn_link(node) == MARK) {
    /* bereits in einem Zyklus besucht. */
    return;
  }
  switch (get_irn_opcode(node)) {
  /* special nodes */
  case iro_Sel:
  case iro_SymConst:
  case iro_Const:
  case iro_Phi:
  case iro_Id:
  case iro_Proj:
  case iro_Tuple:
    /* nothing */
    break;
  /* Sonderbehandlung, da der Funktionszeigereingang nat�rlich kein
   * Verr�ter ist. */
  case iro_Call:
    set_irn_link(node, MARK);
    for (i = get_Call_arity(node) - 1; i >= 0; --i) {
      ir_node * pred = get_Call_param(node, i);
      if (get_irn_mode(pred) == mode_p) {
	free_mark(pred, set);
      }
    }
    break;
  /* other nodes: Alle anderen Knoten nehmen wir als Verr�ter an, bis
   * jemand das Gegenteil implementiert. */
  default:
    set_irn_link(node, MARK);
    for (i = get_irn_arity(node) - 1; i >= 0; --i) {
      ir_node * pred = get_irn_n(node, i);
      if (get_irn_mode(pred) == mode_p) {
	free_mark(pred, set);
      }
    }
    break;
  }
  set_irn_link(node, NULL);
}


/* Die Datenstrukturen f�r sel-Methoden (sel_methods) mu� vor dem
 * Aufruf von "get_free_methods" aufgebaut sein. Die (internen)
 * SymConst-Operationen m�ssen in passende Const-Operationen
 * umgewandelt worden sein, d.h. SymConst-Operationen verweisen immer
 * auf eine echt externe Methode.  */
static entity ** get_free_methods(void) {
  eset * set = eset_create();
  int i;
  entity ** arr = NEW_ARR_F(entity *, 0);
  entity * ent;
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    ir_graph * irg = get_irp_irg(i);
    entity * ent = get_irg_ent(irg);
    /* insert "external visible" methods. */
    if (get_entity_visibility(ent) != local) {
      eset_insert(set, ent);
    }
    irg_walk_graph(irg, NULL, (irg_walk_func) free_ana_walker, set);
  }
  /* Hauptprogramm ist auch frei, auch wenn es nicht "external
   * visible" ist. */
  eset_insert(set, get_irg_ent(get_irp_main_irg()));
  for (ent = eset_first(set); ent; ent = eset_next(set)) {
    ARR_APP1(entity *, arr, ent);
  }
  eset_destroy(set);

  return arr;
}

void cgana(int *length, entity ***free_methods) {
  entity ** free_meths;
  int i;

  sel_methods_init();
  free_meths = get_free_methods();
  callee_ana();
  sel_methods_dispose();

  /* Convert the flexible array to an array that can be handled
     by standard C. */
  *length = ARR_LEN(free_meths);
  *free_methods = (entity **)malloc(sizeof(entity *) * (*length));
  for (i = 0; i < (*length); i++) (*free_methods)[i] = free_meths[i];
  DEL_ARR_F(free_meths);
}
