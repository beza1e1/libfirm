/*
 * Project:     libFIRM
 * File name:   ir/adt/eset.h
 * Purpose:     Datentyp: Vereinfachte Menge (hash-set) zum Speichern von
 *              Zeigern/Adressen.
 * Author:      Hubert Schmid
 * Modified by:
 * Created:     09.06.2002
 * CVS-ID:      $Id$
 * Copyright:   (c) 2002 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */


#ifndef _ESET_H_
#define _ESET_H_

#include <stdbool.h>


/* "eset" ist eine Menge von Adressen. Der Vergleich und das Hashen
 * wird �ber die Adresse gemacht. "NULL" sollte nicht gespeichert
 * werden. */

typedef struct eset eset;


/* Erzeugt eine neue leere Menge. */
eset * eset_create(void);

/* Erzeugt eine Kopie der �bergebenen Menge. Das Kopieren funktioniert
 * nur, wenn in der �bergebenen Menge "NULL" nicht enthalten ist. */
eset * eset_copy(eset *);

/* L�scht die Menge. */
void eset_destroy(eset *);

/* F�gt ein Adresse in die Menge ein, wenn es nicht bereits in der
 * Menge enthalten ist. */
void eset_insert(eset *, void *);

/* Pr�ft ob eine Adresse in der Menge enthalten ist. */
bool eset_contains(eset *, void *);

/* Mit den Funktionen "eset_first" und "eset_next" kann man durch die
 * Menge iterieren. Die Funktion gibt jeweils die Adresse zur�ck. Wenn
 * keine weiteren Adressen in der Menge sind, geben die Funktionen
 * "NULL" zur�ck. Warnung: Man sollte deshalb "NULL" nicht in der
 * Menge speichern, weil man sonst nicht durch die Menge iterieren
 * kann.
 * ACHTUNG: Waehrend dem iterieren darf man keine neuen Elemente
 * einfuergen!! */
void * eset_first(eset *);
void * eset_next(eset *);

/* F�gt alle Elemente der Menge "source" der Menge "target"
 * hinzu. Diese Funktion funktioniert nur, wenn in der Menge "source"
 * die "NULL"-Adresse nicht enthalten ist. */
void eset_insert_all(eset * target, eset * source);


#endif /* _ESET_H_ */
