#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h> // for tolower(), toupper()
#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "lib/mlrregex.h"
#include "lib/mtrand.h"
#include "mapping/mapper.h"
#include "mapping/rval_evaluators.h"
#include "mapping/function_manager.h"
#include "mapping/mlr_dsl_cst.h" // xxx only for allocate_keylist_evaluators_from_ast_node -- xxx move
#include "mapping/context_flags.h"

// ================================================================
// See comments in rval_evaluators.h
// ================================================================

// ----------------------------------------------------------------
rxval_evaluator_t* rxval_evaluator_alloc_from_ast(mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr,
	int type_inferencing, int context_flags)
{
	switch(pnode->type) {

	case MD_AST_NODE_TYPE_MAP_LITERAL:
		return rxval_evaluator_alloc_from_map_literal(
			pnode, pfmgr, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_FUNCTION_CALLSITE:
		return NULL; // xxx XXX mapvar stub
		break;

	case MD_AST_NODE_TYPE_NONINDEXED_LOCAL_VARIABLE:
		return rxval_evaluator_alloc_from_nonindexed_local_variable(
			pnode, pfmgr, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_INDEXED_LOCAL_VARIABLE:
		return rxval_evaluator_alloc_from_indexed_local_variable(
			pnode, pfmgr, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_OOSVAR_KEYLIST:
		return rxval_evaluator_alloc_from_oosvar_keylist(
			pnode, pfmgr, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_FULL_OOSVAR:
		return rxval_evaluator_alloc_from_full_oosvar(
			pnode, pfmgr, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_FULL_SREC:
		return rxval_evaluator_alloc_from_full_srec(
			pnode, pfmgr, type_inferencing, context_flags);
		break;

	default:
		return rxval_evaluator_alloc_wrapping_rval(pnode, pfmgr, type_inferencing, context_flags);
		break;
	}
}

// ================================================================
// xxx
// {
//   "a" : 1,
//   "b" : {
//     "x" : 7,
//     "y" : 8,
//   },
//   "c" : 3,
// }

// $ mlr --from s put -v -q 'm={"a":NR,"b":{"x":999},"c":3};dump m'
// text="block", type=STATEMENT_BLOCK:
//     text="=", type=NONINDEXED_LOCAL_ASSIGNMENT:
//         text="m", type=NONINDEXED_LOCAL_VARIABLE.
//         text="map_literal", type=MAP_LITERAL:
//             text="mappair", type=MAP_LITERAL_PAIR:
//                 text="mapkey", type=MAP_LITERAL:
//                     text="a", type=STRING_LITERAL.
//                 text="mapval", type=MAP_LITERAL:
//                     text="NR", type=CONTEXT_VARIABLE.
//             text="mappair", type=MAP_LITERAL_PAIR:
//                 text="mapkey", type=MAP_LITERAL:
//                     text="b", type=STRING_LITERAL.
//                 text="mapval", type=MAP_LITERAL:
//                     text="map_literal", type=MAP_LITERAL:
//                         text="mappair", type=MAP_LITERAL_PAIR:
//                             text="mapkey", type=MAP_LITERAL:
//                                 text="x", type=STRING_LITERAL.
//                             text="mapval", type=MAP_LITERAL:
//                                 text="999", type=NUMERIC_LITERAL.
//             text="mappair", type=MAP_LITERAL_PAIR:
//                 text="mapkey", type=MAP_LITERAL:
//                     text="c", type=STRING_LITERAL.
//                 text="mapval", type=MAP_LITERAL:
//                     text="3", type=NUMERIC_LITERAL.
//     text="dump", type=DUMP:
//         text=">", type=FILE_WRITE:
//             text="stdout", type=STDOUT:
//         text="m", type=NONINDEXED_LOCAL_VARIABLE.

typedef struct _map_literal_list_evaluator_t {
	sllv_t* ppair_evaluators;
} map_literal_list_evaluator_t;
typedef struct _map_literal_pair_evaluator_t {
	rval_evaluator_t*             pkey_evaluator;
	int                           is_terminal;
	rval_evaluator_t*             pval_evaluator;
	map_literal_list_evaluator_t* plist_evaluator;
} map_literal_pair_evaluator_t;

static map_literal_list_evaluator_t* allocate_map_literal_evaluator_from_ast(
	mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr, int type_inferencing, int context_flags)
{
	map_literal_list_evaluator_t* plist_evaluator = mlr_malloc_or_die(sizeof(map_literal_list_evaluator_t));
	plist_evaluator->ppair_evaluators = sllv_alloc();
	MLR_INTERNAL_CODING_ERROR_IF(pnode->type != MD_AST_NODE_TYPE_MAP_LITERAL);
	for (sllve_t* pe = pnode->pchildren->phead; pe != NULL; pe = pe->pnext) {

		map_literal_pair_evaluator_t* ppair = mlr_malloc_or_die(sizeof(map_literal_pair_evaluator_t));
		*ppair = (map_literal_pair_evaluator_t) {
			.pkey_evaluator  = NULL,
			.is_terminal     = TRUE,
			.pval_evaluator  = NULL,
			.plist_evaluator = NULL,
		};

		mlr_dsl_ast_node_t* pchild = pe->pvvalue;
		MLR_INTERNAL_CODING_ERROR_IF(pchild->type != MD_AST_NODE_TYPE_MAP_LITERAL_PAIR);

		mlr_dsl_ast_node_t* pleft = pchild->pchildren->phead->pvvalue;
		MLR_INTERNAL_CODING_ERROR_IF(pleft->type != MD_AST_NODE_TYPE_MAP_LITERAL_KEY);
		mlr_dsl_ast_node_t* pkeynode = pleft->pchildren->phead->pvvalue;
		ppair->pkey_evaluator = rval_evaluator_alloc_from_ast(pkeynode, pfmgr, type_inferencing, context_flags);

		mlr_dsl_ast_node_t* pright = pchild->pchildren->phead->pnext->pvvalue;
		mlr_dsl_ast_node_t* pvalnode = pright->pchildren->phead->pvvalue;
		if (pright->type == MD_AST_NODE_TYPE_MAP_LITERAL_VALUE) {
			ppair->pval_evaluator = rval_evaluator_alloc_from_ast(pvalnode, pfmgr, type_inferencing, context_flags);
		} else if (pright->type == MD_AST_NODE_TYPE_MAP_LITERAL) {
			ppair->is_terminal = FALSE;
			ppair->plist_evaluator = allocate_map_literal_evaluator_from_ast(
				pvalnode, pfmgr, type_inferencing, context_flags);
		} else {
			MLR_INTERNAL_CODING_ERROR();
		}

		sllv_append(plist_evaluator->ppair_evaluators, ppair);
	}
	return plist_evaluator;
}

// ----------------------------------------------------------------
typedef struct _rxval_evaluator_from_map_literal_state_t {
	map_literal_list_evaluator_t* proot_list_evaluator;
} rxval_evaluator_from_map_literal_state_t;

static void rxval_evaluator_from_map_literal_aux(
	rxval_evaluator_from_map_literal_state_t* pstate,
	map_literal_list_evaluator_t*             plist_evaluator,
	mlhmmv_level_t*                           plevel,
	variables_t*                              pvars)
{
	for (sllve_t* pe = plist_evaluator->ppair_evaluators->phead; pe != NULL; pe = pe->pnext) {
		map_literal_pair_evaluator_t* ppair = pe->pvvalue;

		// mlhmmv_put_terminal_from_level will copy keys and values
		mv_t mvkey = ppair->pkey_evaluator->pprocess_func(ppair->pkey_evaluator->pvstate, pvars);
		if (ppair->is_terminal) {
			sllmve_t e = { .value = mvkey, .free_flags = 0, .pnext = NULL };
			mv_t mvval = ppair->pval_evaluator->pprocess_func(ppair->pval_evaluator->pvstate, pvars);
			mlhmmv_put_terminal_from_level(plevel, &e, &mvval);
		} else {
			sllmve_t e = { .value = mvkey, .free_flags = 0, .pnext = NULL };
			mlhmmv_level_t* pnext_level = mlhmmv_put_empty_map_from_level(plevel, &e);
			rxval_evaluator_from_map_literal_aux(pstate, ppair->plist_evaluator, pnext_level, pvars);
		}
	}
}

mlhmmv_value_t rxval_evaluator_from_map_literal_func(void* pvstate, variables_t* pvars) {
	rxval_evaluator_from_map_literal_state_t* pstate = pvstate;

	mlhmmv_value_t xval = mlhmmv_value_alloc_empty_map();

	rxval_evaluator_from_map_literal_aux(pstate, pstate->proot_list_evaluator, xval.u.pnext_level, pvars);

	return xval;
}

static void rxval_evaluator_from_map_literal_free(rxval_evaluator_t* prxval_evaluator) {
	rxval_evaluator_from_map_literal_state_t* pstate = prxval_evaluator->pvstate;
	//xxx free the tree recursively pstate->prval_evaluator->pfree_func(pstate->prval_evaluator);
	free(pstate);
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_from_map_literal(mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr,
	int type_inferencing, int context_flags)
{
	rxval_evaluator_from_map_literal_state_t* pstate = mlr_malloc_or_die(
		sizeof(rxval_evaluator_from_map_literal_state_t));
	pstate->proot_list_evaluator = allocate_map_literal_evaluator_from_ast(
		pnode, pfmgr, type_inferencing, context_flags);

	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pvstate       = pstate;
	prxval_evaluator->pprocess_func = rxval_evaluator_from_map_literal_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_from_map_literal_free;

	return prxval_evaluator;
}


// ================================================================
typedef struct _rxval_evaluator_from_nonindexed_local_variable_state_t {
	int vardef_frame_relative_index;
} rxval_evaluator_from_nonindexed_local_variable_state_t;

mlhmmv_value_t rxval_evaluator_from_nonindexed_local_variable_func(void* pvstate, variables_t* pvars) {
	rxval_evaluator_from_nonindexed_local_variable_state_t* pstate = pvstate;
	local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
	mlhmmv_value_t* pxval = local_stack_frame_get_map_value(pframe, pstate->vardef_frame_relative_index, NULL);
	if (pxval == NULL) {
		return mlhmmv_value_transfer_terminal(mv_absent()); // xxx rename transfer to wrap ?
	} else {
		return mlhmmv_copy_aux(pxval);
	}
}

static void rxval_evaluator_from_nonindexed_local_variable_free(rxval_evaluator_t* prxval_evaluator) {
	rxval_evaluator_from_nonindexed_local_variable_state_t* pstate = prxval_evaluator->pvstate;
	free(pstate);
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_from_nonindexed_local_variable(
	mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr, int type_inferencing, int context_flags)
{
	rxval_evaluator_from_nonindexed_local_variable_state_t* pstate = mlr_malloc_or_die(
		sizeof(rxval_evaluator_from_nonindexed_local_variable_state_t));
	MLR_INTERNAL_CODING_ERROR_IF(pnode->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstate->vardef_frame_relative_index = pnode->vardef_frame_relative_index;

	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pvstate       = pstate;
	prxval_evaluator->pprocess_func = rxval_evaluator_from_nonindexed_local_variable_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_from_nonindexed_local_variable_free;

	return prxval_evaluator;
}

// ================================================================
typedef struct _rxval_evaluator_from_indexed_local_variable_state_t {
	int vardef_frame_relative_index;
	sllv_t* pkeylist_evaluators;
} rxval_evaluator_from_indexed_local_variable_state_t;

mlhmmv_value_t rxval_evaluator_from_indexed_local_variable_func(void* pvstate, variables_t* pvars) {
	rxval_evaluator_from_indexed_local_variable_state_t* pstate = pvstate;

	int all_non_null_or_error = TRUE;
	sllmv_t* pmvkeys = evaluate_list(pstate->pkeylist_evaluators, pvars, &all_non_null_or_error);

	if (all_non_null_or_error) {
		local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
		mlhmmv_value_t* pxval = local_stack_frame_get_map_value(pframe, pstate->vardef_frame_relative_index,
			pmvkeys);
		sllmv_free(pmvkeys);
		return mlhmmv_copy_aux(pxval);
	} else {
		sllmv_free(pmvkeys);
		return mlhmmv_value_transfer_terminal(mv_absent());
	}
}

static void rxval_evaluator_from_indexed_local_variable_free(rxval_evaluator_t* prxval_evaluator) {
	rxval_evaluator_from_indexed_local_variable_state_t* pstate = prxval_evaluator->pvstate;
	for (sllve_t* pe = pstate->pkeylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
		rval_evaluator_t* prval_evaluator = pe->pvvalue;
		prval_evaluator->pfree_func(prval_evaluator);
	}
	sllv_free(pstate->pkeylist_evaluators);
	free(pstate);
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_from_indexed_local_variable(
	mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr, int type_inferencing, int context_flags)
{
	rxval_evaluator_from_indexed_local_variable_state_t* pstate = mlr_malloc_or_die(
		sizeof(rxval_evaluator_from_indexed_local_variable_state_t));
	MLR_INTERNAL_CODING_ERROR_IF(pnode->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstate->vardef_frame_relative_index = pnode->vardef_frame_relative_index;
	pstate->pkeylist_evaluators = allocate_keylist_evaluators_from_ast_node(
		pnode, pfmgr, type_inferencing, context_flags);

	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pvstate       = pstate;
	prxval_evaluator->pprocess_func = rxval_evaluator_from_indexed_local_variable_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_from_indexed_local_variable_free;

	return prxval_evaluator;
}

// ================================================================
typedef struct _rxval_evaluator_from_oosvar_keylist_state_t {
	sllv_t* pkeylist_evaluators;
} rxval_evaluator_from_oosvar_keylist_state_t;

mlhmmv_value_t rxval_evaluator_from_oosvar_keylist_func(void* pvstate, variables_t* pvars) {
	rxval_evaluator_from_oosvar_keylist_state_t* pstate = pvstate;

	int all_non_null_or_error = TRUE;
	sllmv_t* pmvkeys = evaluate_list(pstate->pkeylist_evaluators, pvars, &all_non_null_or_error);

	if (all_non_null_or_error) {

		int lookup_error = FALSE;
		mlhmmv_value_t* pxval = mlhmmv_get_value_from_level(pvars->poosvars->proot_level,
			pmvkeys, &lookup_error);
		sllmv_free(pmvkeys);
		return mlhmmv_copy_aux(pxval);
	} else {
		sllmv_free(pmvkeys);
		return mlhmmv_value_transfer_terminal(mv_absent());
	}
}

static void rxval_evaluator_from_oosvar_keylist_free(rxval_evaluator_t* prxval_evaluator) {
	rxval_evaluator_from_oosvar_keylist_state_t* pstate = prxval_evaluator->pvstate;
	for (sllve_t* pe = pstate->pkeylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
		rval_evaluator_t* prval_evaluator = pe->pvvalue;
		prval_evaluator->pfree_func(prval_evaluator);
	}
	sllv_free(pstate->pkeylist_evaluators);
	free(pstate);
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_from_oosvar_keylist(
	mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr, int type_inferencing, int context_flags)
{
	rxval_evaluator_from_oosvar_keylist_state_t* pstate = mlr_malloc_or_die(
		sizeof(rxval_evaluator_from_oosvar_keylist_state_t));
	pstate->pkeylist_evaluators = allocate_keylist_evaluators_from_ast_node(
		pnode, pfmgr, type_inferencing, context_flags);

	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pvstate       = pstate;
	prxval_evaluator->pprocess_func = rxval_evaluator_from_oosvar_keylist_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_from_oosvar_keylist_free;

	return prxval_evaluator;
}

// ================================================================
mlhmmv_value_t rxval_evaluator_from_full_oosvar_func(void* pvstate, variables_t* pvars) {
	return mlhmmv_copy_submap_from_root(pvars->poosvars, NULL);
}

static void rxval_evaluator_from_full_oosvar_free(rxval_evaluator_t* prxval_evaluator) {
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_from_full_oosvar(
	mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr, int type_inferencing, int context_flags)
{
	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pprocess_func = rxval_evaluator_from_full_oosvar_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_from_full_oosvar_free;

	return prxval_evaluator;
}

// ================================================================
mlhmmv_value_t rxval_evaluator_from_full_srec_func(void* pvstate, variables_t* pvars) {
	mlhmmv_value_t xval = mlhmmv_value_alloc_empty_map(); // xxx memory leak. replace w/ initter.

	for (lrece_t* pe = pvars->pinrec->phead; pe != NULL; pe = pe->pnext) {
		// mlhmmv_put_terminal_from_level will copy mv keys and values so we needn't (and shouldn't)
		// duplicate them here.
		mv_t k = mv_from_string(pe->key, NO_FREE);
		sllmve_t e = { .value = k, .free_flags = 0, .pnext = NULL };
		mv_t* pomv = lhmsmv_get(pvars->ptyped_overlay, pe->key);
		if (pomv != NULL) {
			mlhmmv_put_terminal_from_level(xval.u.pnext_level, &e, pomv); // xxx make a simpler 1-level API call
		} else {
			mv_t v = mv_from_string(pe->value, NO_FREE); // mlhmmv_put_terminal_from_level will copy
			mlhmmv_put_terminal_from_level(xval.u.pnext_level, &e, &v);
		}
	}

	return xval;
}

static void rxval_evaluator_from_full_srec_free(rxval_evaluator_t* prxval_evaluator) {
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_from_full_srec(
	mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr, int type_inferencing, int context_flags)
{
	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pprocess_func = rxval_evaluator_from_full_srec_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_from_full_srec_free;

	return prxval_evaluator;
}

// ================================================================
typedef struct _rxval_evaluator_wrapping_rval_state_t {
	rval_evaluator_t* prval_evaluator;
} rxval_evaluator_wrapping_rval_state_t;

mlhmmv_value_t rxval_evaluator_wrapping_rval_func(void* pvstate, variables_t* pvars) {
	rxval_evaluator_wrapping_rval_state_t* pstate = pvstate;
	rval_evaluator_t* prval_evaluator = pstate->prval_evaluator;
	mv_t val = prval_evaluator->pprocess_func(prval_evaluator->pvstate, pvars);
	return mlhmmv_value_transfer_terminal(val);
}

static void rxval_evaluator_wrapping_rval_free(rxval_evaluator_t* prxval_evaluator) {
	rxval_evaluator_wrapping_rval_state_t* pstate = prxval_evaluator->pvstate;
	pstate->prval_evaluator->pfree_func(pstate->prval_evaluator);
	free(pstate);
	free(prxval_evaluator);
}

rxval_evaluator_t* rxval_evaluator_alloc_wrapping_rval(mlr_dsl_ast_node_t* pnode, fmgr_t* pfmgr,
	int type_inferencing, int context_flags)
{
	rxval_evaluator_wrapping_rval_state_t* pstate = mlr_malloc_or_die(
		sizeof(rxval_evaluator_wrapping_rval_state_t));
	pstate->prval_evaluator = rval_evaluator_alloc_from_ast(pnode, pfmgr, type_inferencing, context_flags);

	rxval_evaluator_t* prxval_evaluator = mlr_malloc_or_die(sizeof(rxval_evaluator_t));
	prxval_evaluator->pvstate       = pstate;
	prxval_evaluator->pprocess_func = rxval_evaluator_wrapping_rval_func;
	prxval_evaluator->pfree_func    = rxval_evaluator_wrapping_rval_free;

	return prxval_evaluator;
}
