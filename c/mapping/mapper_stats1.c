#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/mlrutil.h"
#include "lib/mlr_globals.h"
#include "cli/argparse.h"
#include "containers/sllv.h"
#include "containers/slls.h"
#include "lib/string_array.h"
#include "containers/lhmslv.h"
#include "containers/lhmsv.h"
#include "containers/mixutil.h"
#include "containers/mlrval.h"
#include "mapping/mappers.h"
#include "mapping/stats1_accumulators.h"

static char* fake_acc_name_for_setups = "__setup_done__";

// ----------------------------------------------------------------
typedef struct _mapper_stats1_state_t {
	ap_state_t* pargp;
	slls_t*         paccumulator_names;
	string_array_t* pvalue_field_names;     // parameter
	string_array_t* pvalue_field_values;    // scratch space used per-record
	slls_t*         pgroup_by_field_names;  // parameter
	lhmslv_t*       groups;
	int             do_iterative_stats;
	int             allow_int_float;
	int             do_interpolated_percentiles;
} mapper_stats1_state_t;

static void      mapper_stats1_usage(FILE* o, char* argv0, char* verb);
static mapper_t* mapper_stats1_parse_cli(int* pargi, int argc, char** argv,
	cli_reader_opts_t* _, cli_writer_opts_t* __);
static mapper_t* mapper_stats1_alloc(ap_state_t* pargp, slls_t* paccumulator_names, string_array_t* pvalue_field_names,
	slls_t* pgroup_by_field_names, int do_iterative_stats, int allow_int_float, int do_interpolated_percentiles);
static void      mapper_stats1_free(mapper_t* pmapper);
static sllv_t*   mapper_stats1_process(lrec_t* pinrec, context_t* pctx, void* pvstate);
static void      mapper_stats1_ingest(lrec_t* pinrec, mapper_stats1_state_t* pstate);
static sllv_t*   mapper_stats1_emit_all(mapper_stats1_state_t* pstate);
static lrec_t*   mapper_stats1_emit(mapper_stats1_state_t* pstate, lrec_t* poutrec,
	char* value_field_name, lhmsv_t* acc_field_to_acc_state_out);

typedef struct _acc_map_pair_t {
	lhmsv_t* pin;
	lhmsv_t* pout;
} acc_map_pair_t;

// ----------------------------------------------------------------
mapper_setup_t mapper_stats1_setup = {
	.verb        = "stats1",
	.pusage_func = mapper_stats1_usage,
	.pparse_func = mapper_stats1_parse_cli,
	.ignores_input = FALSE,
};

// ----------------------------------------------------------------
static void mapper_stats1_usage(FILE* o, char* argv0, char* verb) {
	fprintf(o, "Usage: %s %s [options]\n", argv0, verb);
	fprintf(o, "Computes univariate statistics for one or more given fields, accumulated across\n");
	fprintf(o, "the input record stream.\n");
	fprintf(o, "Options:\n");
	fprintf(o, "-a {sum,count,...}  Names of accumulators: p10 p25.2 p50 p98 p100 etc. and/or\n");
	fprintf(o, "                    one or more of:\n");
	for (int i = 0; i < stats1_acc_lookup_table_length; i++) {
		fprintf(o, "  %-9s %s\n", stats1_acc_lookup_table[i].name, stats1_acc_lookup_table[i].desc);
	}
	fprintf(o, "-f {a,b,c}  Value-field names on which to compute statistics\n");
	fprintf(o, "-g {d,e,f}  Optional group-by-field names\n");
	fprintf(o, "-i          Use interpolated percentiles, like R's type=7; default like type=1.\n");
	fprintf(o, "-s          Print iterative stats. Useful in tail -f contexts (in which\n");
	fprintf(o, "            case please avoid pprint-format output since end of input\n");
	fprintf(o, "            stream will never be seen).\n");
	fprintf(o, "-F          Computes integerable things (e.g. count) in floating point.\n");
	fprintf(o, "Example: %s %s -a min,p10,p50,p90,max -f value -g size,shape\n", argv0, verb);
	fprintf(o, "Example: %s %s -a count,mode -f size\n", argv0, verb);
	fprintf(o, "Example: %s %s -a count,mode -f size -g shape\n", argv0, verb);
	fprintf(o, "Notes:\n");
	fprintf(o, "* p50 is a synonym for median.\n");
	fprintf(o, "* min and max output the same results as p0 and p100, respectively, but use\n");
	fprintf(o, "  less memory.\n");
	fprintf(o, "* count and mode allow text input; the rest require numeric input.\n");
	fprintf(o, "  In particular, 1 and 1.0 are distinct text for count and mode.\n");
	fprintf(o, "* When there are mode ties, the first-encountered datum wins.\n");
}

static mapper_t* mapper_stats1_parse_cli(int* pargi, int argc, char** argv,
	cli_reader_opts_t* _, cli_writer_opts_t* __)
{
	slls_t*         paccumulator_names          = NULL;
	string_array_t* pvalue_field_names          = NULL;
	slls_t*         pgroup_by_field_names       = slls_alloc();
	int             do_iterative_stats          = FALSE;
	int             allow_int_float             = TRUE;
	int             do_interpolated_percentiles = FALSE;

	char* verb = argv[(*pargi)++];

	ap_state_t* pstate = ap_alloc();
	ap_define_string_list_flag(pstate,  "-a", &paccumulator_names);
	ap_define_string_array_flag(pstate, "-f", &pvalue_field_names);
	ap_define_string_list_flag(pstate,  "-g", &pgroup_by_field_names);
	ap_define_true_flag(pstate,         "-s", &do_iterative_stats);
	ap_define_false_flag(pstate,        "-F", &allow_int_float);
	ap_define_true_flag(pstate,         "-i", &do_interpolated_percentiles);

	if (!ap_parse(pstate, verb, pargi, argc, argv)) {
		mapper_stats1_usage(stderr, argv[0], verb);
		return NULL;
	}

	if (paccumulator_names == NULL || pvalue_field_names == NULL) {
		mapper_stats1_usage(stderr, argv[0], verb);
		return NULL;
	}

	return mapper_stats1_alloc(pstate, paccumulator_names, pvalue_field_names, pgroup_by_field_names,
		do_iterative_stats, allow_int_float, do_interpolated_percentiles);
}

// ----------------------------------------------------------------
static mapper_t* mapper_stats1_alloc(ap_state_t* pargp, slls_t* paccumulator_names, string_array_t* pvalue_field_names,
	slls_t* pgroup_by_field_names, int do_iterative_stats, int allow_int_float, int do_interpolated_percentiles)
{
	mapper_t* pmapper = mlr_malloc_or_die(sizeof(mapper_t));

	mapper_stats1_state_t* pstate  = mlr_malloc_or_die(sizeof(mapper_stats1_state_t));

	pstate->pargp                       = pargp;
	pstate->paccumulator_names          = paccumulator_names;
	pstate->pvalue_field_names          = pvalue_field_names;
	pstate->pgroup_by_field_names       = pgroup_by_field_names;
	pstate->pvalue_field_values         = string_array_alloc(pvalue_field_names->length);
	pstate->groups                      = lhmslv_alloc();
	pstate->do_iterative_stats          = do_iterative_stats;
	pstate->allow_int_float             = allow_int_float;
	pstate->do_interpolated_percentiles = do_interpolated_percentiles;

	pmapper->pvstate       = pstate;
	pmapper->pprocess_func = mapper_stats1_process;
	pmapper->pfree_func    = mapper_stats1_free;

	return pmapper;
}

static void mapper_stats1_free(mapper_t* pmapper) {
	mapper_stats1_state_t* pstate = pmapper->pvstate;
	slls_free(pstate->paccumulator_names);
	string_array_free(pstate->pvalue_field_names);
	string_array_free(pstate->pvalue_field_values);
	slls_free(pstate->pgroup_by_field_names);

	// lhmslv_free and lhmsv_free will free the hashmap keys; we need to free
	// the void-star hashmap values.
	for (lhmslve_t* pa = pstate->groups->phead; pa != NULL; pa = pa->pnext) {
		lhmsv_t* pgroup_to_acc_field = pa->pvvalue;
		for (lhmsve_t* pb = pgroup_to_acc_field->phead; pb != NULL; pb = pb->pnext) {
			acc_map_pair_t* pacc_field_to_acc_states = pb->pvvalue;
			lhmsv_t* pacc_field_to_acc_state_in  = pacc_field_to_acc_states->pin;
			lhmsv_t* pacc_field_to_acc_state_out = pacc_field_to_acc_states->pout;
			for (lhmsve_t* pc = pacc_field_to_acc_state_out->phead; pc != NULL; pc = pc->pnext) {
				if (streq(pc->key, fake_acc_name_for_setups))
					continue;
				stats1_acc_t* pstats1_acc = pc->pvvalue;
				pstats1_acc->pfree_func(pstats1_acc);
			}
			lhmsv_free(pacc_field_to_acc_state_in);
			lhmsv_free(pacc_field_to_acc_state_out);
			free(pacc_field_to_acc_states);
		}
		lhmsv_free(pgroup_to_acc_field);
	}
	lhmslv_free(pstate->groups);
	ap_free(pstate->pargp);
	free(pstate);
	free(pmapper);
}

// ================================================================
// Given: accumulate count,sum on values x,y group by a,b.
// Example input:       Example output:
//   a b x y            a b x_count x_sum y_count y_sum
//   s t 1 2            s t 2       6     2       8
//   u v 3 4            u v 1       3     1       4
//   s t 5 6            u w 1       7     1       9
//   u w 7 9
//
// Multilevel hashmap structure:
// {
//   ["s","t"] : {                <--- group-by field names
//     ["x"] : {                  <--- value field names
//       "count" : stats1_count_t object,
//       "sum"   : stats1_sum_t  object
//     },
//     ["y"] : {
//       "count" : stats1_count_t object,
//       "sum"   : stats1_sum_t  object
//     },
//   },
//   ["u","v"] : {
//     ["x"] : {
//       "count" : stats1_count_t object,
//       "sum"   : stats1_sum_t  object
//     },
//     ["y"] : {
//       "count" : stats1_count_t object,
//       "sum"   : stats1_sum_t  object
//     },
//   },
//   ["u","w"] : {
//     ["x"] : {
//       "count" : stats1_count_t object,
//       "sum"   : stats1_sum_t  object
//     },
//     ["y"] : {
//       "count" : stats1_count_t object,
//       "sum"   : stats1_sum_t  object
//     },
//   },
// }
// ================================================================

// In the iterative case, add to the current record its current group's stats fields.
// In the non-iterative case, produce output only at the end of the input stream.
static sllv_t* mapper_stats1_process(lrec_t* pinrec, context_t* pctx, void* pvstate) {
	mapper_stats1_state_t* pstate = pvstate;
	if (pinrec != NULL) {
		mapper_stats1_ingest(pinrec, pstate);
		if (pstate->do_iterative_stats) {
			// The input record is modified in this case, with new fields appended
			return sllv_single(pinrec);
		} else {
			lrec_free(pinrec);
			return NULL;
		}
	} else if (!pstate->do_iterative_stats) {
		return mapper_stats1_emit_all(pstate);
	} else {
		return NULL;
	}
}

// ----------------------------------------------------------------
static void mapper_stats1_ingest(lrec_t* pinrec, mapper_stats1_state_t* pstate) {
	// E.g. ["s", "t"]
	// To do: make value_field_values into a hashmap. Then accept partial
	// population on that, but retain full-population requirement on group-by.
	// E.g. if accumulating stats of x,y on a,b then skip record with x,y,a but
	// process record with x,a,b.
	mlr_reference_values_from_record_into_string_array(pinrec, pstate->pvalue_field_names, pstate->pvalue_field_values);
	slls_t* pgroup_by_field_values = mlr_reference_selected_values_from_record(pinrec, pstate->pgroup_by_field_names);

	if (pgroup_by_field_values == NULL) {
		slls_free(pgroup_by_field_values);
		return;
	}

	lhmsv_t* pgroup_to_acc_field = lhmslv_get(pstate->groups, pgroup_by_field_values);
	if (pgroup_to_acc_field == NULL) {
		pgroup_to_acc_field = lhmsv_alloc();
		lhmslv_put(pstate->groups, slls_copy(pgroup_by_field_values), pgroup_to_acc_field,
			FREE_ENTRY_KEY);
	}

	// for x=1 and y=2
	int n = pstate->pvalue_field_names->length;
	for (int i = 0; i < n; i++) {
		char* value_field_name = pstate->pvalue_field_names->strings[i];
		char* value_field_sval = pstate->pvalue_field_values->strings[i];

		// For percentiles there is one unique accumulator given (for example) five distinct
		// names p0,p25,p50,p75,p100.  The input accumulators are unique: only one
		// percentile-keeper. There are multiple output accumulators: each references the same
		// underlying percentile-keeper but with distinct parameters.  Hence the ->pin and ->pout maps.
		acc_map_pair_t* pacc_field_to_acc_states = lhmsv_get(pgroup_to_acc_field, value_field_name);
		if (pacc_field_to_acc_states == NULL) {
			pacc_field_to_acc_states = mlr_malloc_or_die(sizeof(acc_map_pair_t));
			pacc_field_to_acc_states->pin  = lhmsv_alloc();
			pacc_field_to_acc_states->pout = lhmsv_alloc();
			lhmsv_put(pgroup_to_acc_field, value_field_name, pacc_field_to_acc_states, NO_FREE);
		}
		lhmsv_t* acc_field_to_acc_state_in  = pacc_field_to_acc_states->pin;
		lhmsv_t* acc_field_to_acc_state_out = pacc_field_to_acc_states->pout;

		// Look up presence of all accumulators at this level's hashmap.
		char* presence = lhmsv_get(acc_field_to_acc_state_in, fake_acc_name_for_setups);
		if (presence == NULL) {
			make_stats1_accs(value_field_name, pstate->paccumulator_names, pstate->allow_int_float,
				pstate->do_interpolated_percentiles, acc_field_to_acc_state_in, acc_field_to_acc_state_out);
			lhmsv_put(acc_field_to_acc_state_in, fake_acc_name_for_setups, fake_acc_name_for_setups, NO_FREE);
		}

		if (value_field_sval == NULL) // Key not present
			continue;
		if (*value_field_sval == 0) // Key present with null value
			continue;

		int have_dval = FALSE;
		int have_nval = FALSE;
		double value_field_dval = -999.0;
		mv_t   value_field_nval = mv_absent();

		// There isn't a one-to-one mapping between user-specified stats1_names
		// and internal stats1_acc_t's. Here in the ingestor we feed each datum
		// into a stats1_acc_t.  In the emitter, we loop over the stats1_names in
		// user-specified order. Example: they ask for p10,mean,p90. Then there
		// is only one percentiles accumulator to be told about each point. In
		// the emitter it will be asked to produce output twice: once for the
		// 10th percentile & once for the 90th.
		for (lhmsve_t* pc = acc_field_to_acc_state_in->phead; pc != NULL; pc = pc->pnext) {
			char* stats1_acc_name = pc->key;
			if (streq(stats1_acc_name, fake_acc_name_for_setups))
				continue;
			stats1_acc_t* pstats1_acc = pc->pvvalue;

			if (pstats1_acc->pdingest_func != NULL) {
				if (!have_dval) {
					value_field_dval = mlr_double_from_string_or_die(value_field_sval);
					have_dval = TRUE;
				}
				pstats1_acc->pdingest_func(pstats1_acc->pvstate, value_field_dval);
			}
			if (pstats1_acc->pningest_func != NULL) {
				if (!have_nval) {
					value_field_nval = pstate->allow_int_float
						? mv_scan_number_or_die(value_field_sval)
						: mv_from_float(mlr_double_from_string_or_die(value_field_sval));
					have_nval = TRUE;
				}
				pstats1_acc->pningest_func(pstats1_acc->pvstate, &value_field_nval);
			}
			if (pstats1_acc->psingest_func != NULL) {
				pstats1_acc->psingest_func(pstats1_acc->pvstate, value_field_sval);
			}

		}
		if (pstate->do_iterative_stats) {
			mapper_stats1_emit(pstate, pinrec, value_field_name, acc_field_to_acc_state_out);
		}
	}
	slls_free(pgroup_by_field_values);
}

// ----------------------------------------------------------------
static sllv_t* mapper_stats1_emit_all(mapper_stats1_state_t* pstate) {
	sllv_t* poutrecs = sllv_alloc();

	for (lhmslve_t* pa = pstate->groups->phead; pa != NULL; pa = pa->pnext) {
		slls_t* pgroup_by_field_values = pa->key;
		lrec_t* poutrec = lrec_unbacked_alloc();

		// Add in a=s,b=t fields:
		sllse_t* pb = pstate->pgroup_by_field_names->phead;
		sllse_t* pc =         pgroup_by_field_values->phead;
		for ( ; pb != NULL && pc != NULL; pb = pb->pnext, pc = pc->pnext) {
			lrec_put(poutrec, pb->value, pc->value, NO_FREE);
		}

		// Add in fields such as x_sum=#, y_count=#, etc.:
		lhmsv_t* pgroup_to_acc_field = pa->pvvalue;
		// for "x", "y"
		for (lhmsve_t* pd = pgroup_to_acc_field->phead; pd != NULL; pd = pd->pnext) {
			char* value_field_name = pd->key;
			acc_map_pair_t* pacc_field_to_acc_states = pd->pvvalue;
			lhmsv_t* acc_field_to_acc_state_out = pacc_field_to_acc_states->pout;
			mapper_stats1_emit(pstate, poutrec, value_field_name, acc_field_to_acc_state_out);
		}
		sllv_append(poutrecs, poutrec);
	}
	sllv_append(poutrecs, NULL);
	return poutrecs;
}

// ----------------------------------------------------------------
static lrec_t* mapper_stats1_emit(mapper_stats1_state_t* pstate, lrec_t* poutrec,
	char* value_field_name, lhmsv_t* acc_field_to_acc_state_out)
{
	// Add in fields such as x_sum=#, y_count=#, etc.:
	for (sllse_t* pe = pstate->paccumulator_names->phead; pe != NULL; pe = pe->pnext) {
		char* stats1_acc_name = pe->value;
		if (streq(stats1_acc_name, fake_acc_name_for_setups))
			continue;
		stats1_acc_t* pstats1_acc = lhmsv_get(acc_field_to_acc_state_out, stats1_acc_name);
		MLR_INTERNAL_CODING_ERROR_IF(pstats1_acc == NULL);
		pstats1_acc->pemit_func(pstats1_acc->pvstate, value_field_name, stats1_acc_name, FALSE, poutrec);
	}
	return poutrec;
}
