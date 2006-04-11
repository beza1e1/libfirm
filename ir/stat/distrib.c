/*
 * Project:     libFIRM
 * File name:   ir/ir/distrib.c
 * Purpose:     Statistics for Firm. Distribution tables.
 * Author:      Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "hashptr.h"
#include "irtools.h"
#include "xmalloc.h"
#include "firmstat_t.h"

static const counter_t _cnt_0 = { { 0 } };

/**
 * calculates a hash value for an address
 */
static unsigned addr_hash(const void *object)
{
  return HASH_PTR(object);
}

/**
 * calculates a hash value for an integer
 */
static unsigned int_hash(const void *object)
{
  return (unsigned)PTR_TO_INT(object);
}

/**
 * compare function for integer distribution tables
 */
static int int_cmp_fun(const void *elt, const void *key)
{
  const distrib_entry_t *p1 = elt;
  const distrib_entry_t *p2 = key;

  return (int)(p1->object) - (int)(p2->object);
}

/*
 * create a new distribution table
 */
distrib_tbl_t *stat_new_distrib_tbl(pset_cmp_fun cmp_func, distrib_hash_fun hash_func)
{
  distrib_tbl_t *res;

  res = xmalloc(sizeof(*res));

  obstack_init(&res->cnts);

  /* create the hash-table */
  res->hash_map  = new_pset(cmp_func, 8);
  res->hash_func = hash_func ? hash_func : addr_hash;
  res->int_dist  = 0;

  return res;
}

/*
 * create a new distribution table for an integer distribution
 */
distrib_tbl_t *stat_new_int_distrib_tbl(void)
{
  distrib_tbl_t *res = stat_new_distrib_tbl(int_cmp_fun, int_hash);

  if (res)
    res->int_dist = 1;

  return res;
}

/*
 * destroy a distribution table
 */
void stat_delete_distrib_tbl(distrib_tbl_t *tbl)
{
  if (tbl) {
    /* free all entries */
    obstack_free(&tbl->cnts, NULL);

    /* delete the hash table */
    del_pset(tbl->hash_map);
  }
}

/**
 * Returns the associates distrib_entry_t for an object
 */
static distrib_entry_t *distrib_get_entry(distrib_tbl_t *tbl, const void *object)
{
  distrib_entry_t key;
  distrib_entry_t *elem;

  key.object = object;

  elem = pset_find(tbl->hash_map, &key, tbl->hash_func(object));
  if (elem)
    return elem;

  elem = obstack_alloc(&tbl->cnts, sizeof(*elem));

  /* clear counter */
  cnt_clr(&elem->cnt);

  elem->object = object;

  return pset_insert(tbl->hash_map, elem, tbl->hash_func(object));
}

/*
 * adds a new object count into the distribution table
 */
void stat_add_distrib_tbl(distrib_tbl_t *tbl, const void *object, const counter_t *cnt)
{
  distrib_entry_t *elem = distrib_get_entry(tbl, object);

  cnt_add(&elem->cnt, cnt);
}

/*
 * adds a new key count into the integer distribution table
 */
void stat_add_int_distrib_tbl(distrib_tbl_t *tbl, int key, const counter_t *cnt)
{
  stat_add_distrib_tbl(tbl, (const void *)key, cnt);
}

/*
 * increases object count by one
 */
void stat_inc_distrib_tbl(distrib_tbl_t *tbl, const void *object)
{
  distrib_entry_t *elem = distrib_get_entry(tbl, object);

  cnt_inc(&elem->cnt);
}

/*
 * increases key count by one
 */
void stat_inc_int_distrib_tbl(distrib_tbl_t *tbl, int key)
{
  stat_inc_distrib_tbl(tbl, (const void *)key);
}


/*
 * inserts a new object with count 0 into the distribution table
 * if object is already present, nothing happens
 */
void stat_insert_distrib_tbl(distrib_tbl_t *tbl, const void *object)
{
  distrib_entry_t *elem = distrib_get_entry(tbl, object);

  cnt_add(&elem->cnt, &_cnt_0);
}

/*
 * inserts a new key with count 0 into the integer distribution table
 * if key is already present, nothing happens
 */
void stat_insert_int_distrib_tbl(distrib_tbl_t *tbl, int key)
{
  stat_insert_distrib_tbl(tbl, (const void *)key);
}

/*
 * returns the sum over all counters in a distribution table
 */
int stat_get_count_distrib_tbl(distrib_tbl_t *tbl)
{
  distrib_entry_t *entry;
  int sum = 0;

  for (entry = pset_first(tbl->hash_map); entry; entry = pset_next(tbl->hash_map)) {
    sum += cnt_to_int(&entry->cnt);
  }

  return sum;
}

/*
 * calculates the mean value of a distribution
 */
double stat_calc_mean_distrib_tbl(distrib_tbl_t *tbl)
{
  distrib_entry_t *entry;
  unsigned count;
  double sum;

  if (tbl->int_dist) {
    /* integer distribution, need min, max */
    int min, max;

    entry = pset_first(tbl->hash_map);

    if (! entry)
      return 0.0;

    min =
    max = (int)entry->object;
    sum = cnt_to_dbl(&entry->cnt);


    for (entry = pset_next(tbl->hash_map); entry; entry = pset_next(tbl->hash_map)) {
      int value = (int)entry->object;

      if (value < min)
        min = value;
      if (value > max);
        max = value;

      sum += cnt_to_dbl(&entry->cnt);
    }
    count = max - min + 1;
  }
  else {
    sum = 0.0;
    count = 0;
    for (entry = pset_first(tbl->hash_map); entry; entry = pset_next(tbl->hash_map)) {
      sum += cnt_to_dbl(&entry->cnt);
      ++count;
    }
  }

  return count ? sum / (double)count : 0.0;
}

/*
 * calculates the average value of a distribution
 */
double stat_calc_avg_distrib_tbl(distrib_tbl_t *tbl)
{
  distrib_entry_t *entry;
  unsigned         count = 0;
  double           sum   = 0.0;

  if (tbl->int_dist) {
    entry = pset_first(tbl->hash_map);

    if (! entry)
      return 0.0;

    sum   = cnt_to_dbl(&entry->cnt);
	count = cnt_to_int(&entry->cnt);

    for (entry = pset_next(tbl->hash_map); entry; entry = pset_next(tbl->hash_map)) {
      sum   += cnt_to_dbl(&entry->cnt) * (int)entry->object;
      count += cnt_to_int(&entry->cnt);
    }
  }
  else {
    for (entry = pset_first(tbl->hash_map); entry; entry = pset_next(tbl->hash_map)) {
      sum += cnt_to_dbl(&entry->cnt);
      ++count;
    }
  }

  return count ? sum / (double)count : 0.0;
}

/**
 * iterates over all entries in a distribution table
 */
void stat_iterate_distrib_tbl(distrib_tbl_t *tbl, eval_distrib_entry_fun eval, void *env)
{
  distrib_entry_t *entry;

  for (entry = pset_first(tbl->hash_map); entry; entry = pset_next(tbl->hash_map)) {
    eval(entry, env);
  }
}
