/* -------------------------------------------------------------------
 * $Id$
 * -------------------------------------------------------------------
 * Datentyp: Vereinfachte Map (hash-map) zum Speichern von
 * Zeigern/Adressen -> Zeigern/Adressen.
 *
 * Erstellt: Hubert Schmid, 09.06.2002
 * ---------------------------------------------------------------- */


#ifndef _PMAP_H_
#define _PMAP_H_


#include "bool.h"


/* Map die Adressen auf Adressen abbildet. Der Vergleich und das
 * Hashen findet �ber die Adresse statt. */

typedef struct pmap pmap;

typedef struct pmap_entry {
  void * key;
  void * value;
} pmap_entry;


/* Erzeugt eine neue leere Map. */
pmap * pmap_create(void);

/* L�scht eine Map. */
void pmap_destroy(pmap *);

/* F�gt ein Paar (key,value) in die Map ein. Gibt es bereits einen
 * Eintrag mit "key" in er Map, so wird der entsprechende "value"
 * �berschrieben. */
void pmap_insert(pmap *, void * key, void * value);

/* Pr�ft ob ein Eintrag zu "key" exisitiert. */
bool pmap_contains(pmap *, void * key);

/* Gibt den Eintrag zu "key" zur�ck. */
pmap_entry * pmap_find(pmap *, void * key);

/* Gibt f�r den Eintrag zu "key" den "value" zur�ck. */
void * pmap_get(pmap *, void * key);

/* Mit den Funktionen "pmap_first" und "pmap_next" kann man durch die
 * Map iterieren. Die Funktionen geben einen Zeiger auf einen Eintrag
 * zur�ck (key,value). Die Funktionen geben "NULL" zur�ck, wenn kein
 * weiterer Eintrag existiert. */
pmap_entry * pmap_first(pmap *);
pmap_entry * pmap_next(pmap *);


#endif /* _PMAP_H_ */
