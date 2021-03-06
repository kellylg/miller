// ================================================================
// Array-only (open addressing) string-to-mlrval linked hash map with linear
// probing for collisions.
//
// Keys and values are not strduped.
//
// John Kerl 2012-08-13
//
// Notes:
// * null key is not supported.
// * null value is supported.
//
// See also:
// * http://en.wikipedia.org/wiki/Hash_table
// * http://docs.oracle.com/javase/6/docs/api/java/util/Map.html
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "containers/lhmsmv.h"
#include "containers/free_flags.h"

// ----------------------------------------------------------------
// Allow compile-time override, e.g using gcc -D.
#ifndef INITIAL_ARRAY_LENGTH
#define INITIAL_ARRAY_LENGTH 32
#endif

#ifndef LOAD_FACTOR
#define LOAD_FACTOR          0.7
#endif

#ifndef ENLARGEMENT_FACTOR
#define ENLARGEMENT_FACTOR   2
#endif

// ----------------------------------------------------------------
#define OCCUPIED 0xa4
#define DELETED  0xb8
#define EMPTY    0xce

// ----------------------------------------------------------------
static void lhmsmv_put_no_enlarge(lhmsmv_t* pmap, char* key, mv_t* pvalue, char free_flags);
static void lhmsmv_enlarge(lhmsmv_t* pmap);

static void lhmsmv_init(lhmsmv_t *pmap, int length) {
	pmap->num_occupied = 0;
	pmap->num_freed    = 0;
	pmap->array_length = length;

	pmap->entries      = (lhmsmve_t*)mlr_malloc_or_die(sizeof(lhmsmve_t) * length);
	// Don't do a memset of all entries at init time, since this has a drastic
	// effect on the time needed to construct an empty map (and Miller
	// constructs an awful lot of those). The attributes there are don't-cares
	// if the corresponding entry state is EMPTY. They are set on put, and
	// mutated on remove.

	pmap->states = (lhmsmve_state_t*)mlr_malloc_or_die(sizeof(lhmsmve_state_t) * length);
	memset(pmap->states, EMPTY, length);

	pmap->phead = NULL;
	pmap->ptail = NULL;
}

lhmsmv_t* lhmsmv_alloc() {
	lhmsmv_t* pmap = mlr_malloc_or_die(sizeof(lhmsmv_t));
	lhmsmv_init(pmap, INITIAL_ARRAY_LENGTH);
	return pmap;
}

// ----------------------------------------------------------------
lhmsmv_t* lhmsmv_copy(lhmsmv_t* pold) {
	lhmsmv_t* pnew = lhmsmv_alloc();

	for (lhmsmve_t* pe = pold->phead; pe != NULL; pe = pe->pnext) {
		char* nkey = mlr_strdup_or_die(pe->key);
		mv_t  nval = mv_copy(&pe->value);
		lhmsmv_put(pnew, nkey, &nval, FREE_ENTRY_KEY | FREE_ENTRY_VALUE);
	}

	return pnew;
}

// ----------------------------------------------------------------
void lhmsmv_clear(lhmsmv_t* pmap) {
	if (pmap == NULL)
		return;
	for (lhmsmve_t* pe = pmap->phead; pe != NULL; pe = pe->pnext) {
		if (pe->free_flags & FREE_ENTRY_KEY)
			free(pe->key);
		if (pe->free_flags & FREE_ENTRY_VALUE)
			mv_free(&pe->value);
	}
	pmap->num_occupied = 0;
	pmap->num_freed    = 0;
	memset(pmap->states, EMPTY, pmap->array_length);
	pmap->phead        = NULL;
	pmap->ptail        = NULL;
}

// ----------------------------------------------------------------
void lhmsmv_free(lhmsmv_t* pmap) {
	if (pmap == NULL)
		return;
	for (lhmsmve_t* pe = pmap->phead; pe != NULL; pe = pe->pnext) {
		if (pe->free_flags & FREE_ENTRY_KEY)
			free(pe->key);
		if (pe->free_flags & FREE_ENTRY_VALUE)
			mv_free(&pe->value);
	}
	free(pmap->entries);
	free(pmap->states);
	pmap->entries      = NULL;
	pmap->num_occupied = 0;
	pmap->num_freed    = 0;
	pmap->array_length = 0;
	free(pmap);
}

// ----------------------------------------------------------------
// Used by get() and remove().
// Returns >=0 for where the key is *or* should go (end of chain).
static int lhmsmv_find_index_for_key(lhmsmv_t* pmap, char* key, int* pideal_index) {
	int hash = mlr_string_hash_func(key);
	int index = mlr_canonical_mod(hash, pmap->array_length);
	*pideal_index = index;
	int num_tries = 0;

	while (TRUE) {
		lhmsmve_t* pe = &pmap->entries[index];
		if (pmap->states[index] == OCCUPIED) {
			char* ekey = pe->key;
			// Existing key found in chain.
			if (streq(key, ekey))
				return index;
		}
		else if (pmap->states[index] == EMPTY) {
			return index;
		}

		// If the current entry has been freed, i.e. previously occupied,
		// the sought index may be further down the chain.  So we must
		// continue looking.
		if (++num_tries >= pmap->array_length) {
			fprintf(stderr,
				"%s: internal coding error: table full even after enlargement.\n", MLR_GLOBALS.bargv0);
			exit(1);
		}

		// Linear probing.
		if (++index >= pmap->array_length)
			index = 0;
	}
	MLR_INTERNAL_CODING_ERROR();
	return -1; // not reached
}

// ----------------------------------------------------------------
void lhmsmv_put(lhmsmv_t* pmap, char* key, mv_t* pvalue, char free_flags) {
	if ((pmap->num_occupied + pmap->num_freed) >= (pmap->array_length*LOAD_FACTOR))
		lhmsmv_enlarge(pmap);
	lhmsmv_put_no_enlarge(pmap, key, pvalue, free_flags);
}

static void lhmsmv_put_no_enlarge(lhmsmv_t* pmap, char* key, mv_t* pvalue, char free_flags) {
	int ideal_index = 0;
	int index = lhmsmv_find_index_for_key(pmap, key, &ideal_index);
	lhmsmve_t* pe = &pmap->entries[index];

	if (pmap->states[index] == OCCUPIED) {
		// Existing key found in chain; put value.
		if (pe->free_flags & FREE_ENTRY_VALUE)
			mv_free(&pe->value);
		pe->value = *pvalue;
		if (free_flags & FREE_ENTRY_VALUE)
			pe->free_flags |= FREE_ENTRY_VALUE;
		else
			pe->free_flags &= ~FREE_ENTRY_VALUE;

		// The caller asked us to free the key when we were done but another copy of the
		// key is already present. So free now what they passed in.
		if (free_flags & FREE_ENTRY_KEY)
			free(key);
	} else if (pmap->states[index] == EMPTY) {
		// End of chain.
		pe->ideal_index = ideal_index;
		pe->key = key;
		pe->value = *pvalue;
		pe->free_flags = free_flags;
		pmap->states[index] = OCCUPIED;

		if (pmap->phead == NULL) {
			pe->pprev   = NULL;
			pe->pnext   = NULL;
			pmap->phead = pe;
			pmap->ptail = pe;
		} else {
			pe->pprev   = pmap->ptail;
			pe->pnext   = NULL;
			pmap->ptail->pnext = pe;
			pmap->ptail = pe;
		}
		pmap->num_occupied++;

	} else {
		fprintf(stderr, "%s: lhmsmv_find_index_for_key did not find end of chain.\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
mv_t* lhmsmv_get(lhmsmv_t* pmap, char* key) {
	int ideal_index = 0;
	int index = lhmsmv_find_index_for_key(pmap, key, &ideal_index);
	lhmsmve_t* pe = &pmap->entries[index];

	if (pmap->states[index] == OCCUPIED) {
		return &pe->value;
	} else if (pmap->states[index] == EMPTY) {
		return NULL;
	} else {
		fprintf(stderr, "%s: lhmsmv_find_index_for_key did not find end of chain.\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
int lhmsmv_has_key(lhmsmv_t* pmap, char* key) {
	int ideal_index = 0;
	int index = lhmsmv_find_index_for_key(pmap, key, &ideal_index);

	if (pmap->states[index] == OCCUPIED)
		return TRUE;
	else if (pmap->states[index] == EMPTY)
		return FALSE;
	else {
		fprintf(stderr, "%s: lhmsmv_find_index_for_key did not find end of chain.\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
static void lhmsmv_enlarge(lhmsmv_t* pmap) {
	lhmsmve_t*       old_entries = pmap->entries;
	lhmsmve_state_t* old_states  = pmap->states;
	lhmsmve_t*       old_head    = pmap->phead;

	lhmsmv_init(pmap, pmap->array_length*ENLARGEMENT_FACTOR);

	for (lhmsmve_t* pe = old_head; pe != NULL; pe = pe->pnext) {
		lhmsmv_put_no_enlarge(pmap, pe->key, &pe->value, pe->free_flags);
	}
	free(old_entries);
	free(old_states);
}

// ----------------------------------------------------------------
void lhmsmv_dump(lhmsmv_t* pmap) {
	for (lhmsmve_t* pe = pmap->phead; pe != NULL; pe = pe->pnext) {
		const char* key_string = (pe == NULL) ? "none" :
			pe->key == NULL ? "null" :
			pe->key;
		char* value_string = mv_alloc_format_val(&pe->value);
		printf("| prev: %p curr: %p next: %p | nidx: %6d | key: %12s | value: %12s |\n",
			pe->pprev, pe, pe->pnext,
			pe->ideal_index, key_string, value_string);
	}
}

// ----------------------------------------------------------------
int lhmsmv_check_counts(lhmsmv_t* pmap) {
	int nocc = 0;
	int ndel = 0;
	for (int index = 0; index < pmap->array_length; index++) {
		if (pmap->states[index] == OCCUPIED)
			nocc++;
		else if (pmap->states[index] == DELETED)
			ndel++;
	}
	if (nocc != pmap->num_occupied) {
		fprintf(stderr,
			"occupancy-count mismatch:  actual %d != cached  %d.\n",
				nocc, pmap->num_occupied);
		return FALSE;
	}
	if (ndel != pmap->num_freed) {
		fprintf(stderr,
			"deleted-count mismatch:  actual %d != cached  %d.\n",
				ndel, pmap->num_freed);
		return FALSE;
	}
	return TRUE;
}
