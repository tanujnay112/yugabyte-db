/*-------------------------------------------------------------------------
 *
 * nodeNestloop.c
 *	  routines to support nest-loop joins
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeNestloop.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecNestLoop	 - process a nestloop join of two plans
 *		ExecInitNestLoop - initialize the join
 *		ExecEndNestLoop  - shut down the join
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeNestloop.h"
#include "nodes/relation.h"
#include "miscadmin.h"
#include "utils/memutils.h"

bool CreateBatch(NestLoopState *node, ExprContext *econtext);
int GetBatchSize(NestLoop *node);
bool IsBatched(NestLoop *node);

bool FlushTuple(NestLoopState *node, ExprContext *econtext);
bool GetNewOuterTuple(NestLoopState *node, ExprContext *econtext);
void ResetBatch(NestLoopState *node, ExprContext *econtext);
void RegisterOuterMatch(NestLoopState *node, ExprContext *econtext);
void AddTupleToOuterBatch(NestLoopState *node, TupleTableSlot *slot);
void FreeBatch(NestLoopState *node);

bool FlushTupleHash(NestLoopState *node, ExprContext *econtext);
bool GetNewOuterTupleHash(NestLoopState *node, ExprContext *econtext);
void ResetBatchHash(NestLoopState *node, ExprContext *econtext);
void RegisterOuterMatchHash(NestLoopState *node, ExprContext *econtext);
void AddTupleToOuterBatchHash(NestLoopState *node, TupleTableSlot *slot);
void FreeBatchHash(NestLoopState *node);


// #define USE_HASH

// #ifdef USE_HASH
// #define LOCAL_JOIN_METHOD(x) x##Hash
// #else
// #define LOCAL_JOIN_METHOD(x) x
// #endif

#define LOCAL_JOIN_METHOD_0(fn, node) (*node->fn##Impl)(node)
#define LOCAL_JOIN_METHOD(fn, node, ...) (*node->fn##Impl)(node, __VA_ARGS__)


/* ----------------------------------------------------------------
 *		ExecNestLoop(node)
 *
 * old comments
 *		Returns the tuple joined from inner and outer tuples which
 *		satisfies the qualification clause.
 *
 *		It scans the inner relation to join with current outer tuple.
 *
 *		If none is found, next tuple from the outer relation is retrieved
 *		and the inner relation is scanned from the beginning again to join
 *		with the outer tuple.
 *
 *		NULL is returned if all the remaining outer tuples are tried and
 *		all fail to join with the inner tuples.
 *
 *		NULL is also returned if there is no tuple from inner relation.
 *
 *		Conditions:
 *		  -- outerTuple contains current tuple from outer relation and
 *			 the right son(inner relation) maintains "cursor" at the tuple
 *			 returned previously.
 *				This is achieved by maintaining a scan position on the outer
 *				relation.
 *
 *		Initial States:
 *		  -- the outer child and the inner child
 *			   are prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecNestLoop(PlanState *pstate)
{
	NestLoopState *node = castNode(NestLoopState, pstate);
	NestLoop   *nl;
	PlanState  *innerPlan;
	// PlanState  *outerPlan;
	// TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;
	ExprState  *joinqual;
	ExprState  *otherqual;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	/*
	 * get information from the node
	 */
	ENL1_printf("getting info from node");

	nl = (NestLoop *) node->js.ps.plan;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	// outerPlan = outerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;
	innerPlan = innerPlanState(node);

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * Ok, everything is setup for the join so now loop until we return a
	 * qualifying join tuple.
	 */
	ENL1_printf("entering main loop");

	for (;;)
	{
		bool success = false;
		Assert(IsBatched(nl) || node->nl_NeedNewInner);
		switch (node->nl_currentstatus)
		{
			case NL_INIT:
				node->nl_currentstatus = NL_BATCHING;
				node->nl_NeedNewInner = true;
				node->nl_NeedNewOuter = true;
				node->nl_MatchedOuter = false;
				switch_fallthrough();
			case NL_NOBATCH:
				Assert(node->nl_NeedNewOuter || node->nl_NeedNewInner);
				if (node->nl_NeedNewOuter)
				{
					success = CreateBatch(node, econtext);
					if (!success)
						return NULL;
					
					node->nl_NeedNewInner = true;
					node->nl_MatchedOuter = false;

					/*
					* now rescan the inner plan
					*/
					ENL1_printf("rescanning inner plan");
					ExecReScan(innerPlan);
					/*
					* we have an outerTuple, try to get the next inner tuple.
					*/
					ENL1_printf("getting new inner tuple");

				}
				switch_fallthrough();
			case NL_BATCHING:
				/*
				* we have an outerTuple, try to get the next inner tuple.
				*/
				ENL1_printf("getting new inner tuple");
				Assert(node->nl_currentstatus == NL_NOBATCH
					   || node->nl_NeedNewInner
					   || !TupIsNull(econtext->ecxt_innertuple));

				if (node->nl_NeedNewInner
					|| node->nl_currentstatus == NL_NOBATCH)
				{
					innerTupleSlot = ExecProcNode(innerPlan);
					econtext->ecxt_innertuple = innerTupleSlot;
					node->nl_NeedNewInner = false;

					if (node->nl_currentstatus == NL_NOBATCH)
						break;

					Assert(IsBatched(nl));
					LOCAL_JOIN_METHOD(ResetBatch, node, econtext);

					if (TupIsNull(innerTupleSlot))
					{
						node->nl_currentstatus = NL_FLUSHING;
						continue;
					}

					/* Why would you ever want a new inner tuple but no new
					 * outer tuple */
					Assert(node->nl_NeedNewOuter);
				}

				if (node->nl_NeedNewOuter)
				{
					Assert(IsBatched(nl));
					Assert(!TupIsNull(econtext->ecxt_innertuple));

					if(!LOCAL_JOIN_METHOD(GetNewOuterTuple, node, econtext))
					{
						node->nl_NeedNewInner = true;
						continue;
					}
					break;
				}
				break;
			case NL_FLUSHING:
				if (nl->join.jointype == JOIN_INNER
					|| nl->join.jointype == JOIN_SEMI)
				{
					node->nl_currentstatus = NL_INIT;
					continue;
				}
				success = LOCAL_JOIN_METHOD(FlushTuple, node, econtext);
				if (success)
				{
					node->nl_MatchedOuter = false;
					break;
				}
				/* tuplestate should be clean */
				node->nl_currentstatus = NL_INIT;
				continue;
			default:
				Assert(false);
		}

		innerTupleSlot = econtext->ecxt_innertuple;

		if (TupIsNull(innerTupleSlot) || node->nl_currentstatus == NL_FLUSHING)
		{
			ENL1_printf("no inner tuple, need new outer tuple");

			node->nl_NeedNewOuter = true;
			node->nl_NeedNewInner = true;

			if (!node->nl_MatchedOuter &&
				(node->js.jointype == JOIN_LEFT ||
				 node->js.jointype == JOIN_ANTI))
			{
				/*
				 * We are doing an outer join and there were no join matches
				 * for this outer tuple.  Generate a fake join tuple with
				 * nulls for the inner tuple, and return it if it passes the
				 * non-join quals.
				 */
				econtext->ecxt_innertuple = node->nl_NullInnerTupleSlot;

				ENL1_printf("testing qualification for outer-join tuple");

				if (otherqual == NULL || ExecQual(otherqual, econtext))
				{
					/*
					 * qualification was satisfied so we project and return
					 * the slot containing the result tuple using
					 * ExecProject().
					 */
					ENL1_printf("qualification succeeded, projecting tuple");

					return ExecProject(node->js.ps.ps_ProjInfo);
				}
				else
					InstrCountFiltered2(node, 1);
			}

			/*
			 * Otherwise just return to top of loop for a new outer tuple.
			 */
			continue;
		}

		node->nl_NeedNewOuter = IsBatched(nl);
		node->nl_NeedNewInner = !IsBatched(nl);

		/*
		 * at this point we have a new pair of inner and outer tuples so we
		 * test the inner and outer tuples to see if they satisfy the node's
		 * qualification.
		 *
		 * Only the joinquals determine MatchedOuter status, but all quals
		 * must pass to actually return the tuple.
		 */
		ENL1_printf("testing qualification");

		if (ExecQual(joinqual, econtext))
		{
			if (IsBatched(nl))
				LOCAL_JOIN_METHOD(RegisterOuterMatch, node, econtext);
			node->nl_MatchedOuter = true;
			/* In an antijoin, we never return a matched tuple */

			if (node->js.jointype == JOIN_ANTI)
			{
				/*
				 * This outer tuple has been matched so never think about 
				 * this outer tuple again.
				 */
				node->nl_NeedNewOuter = true;
				continue;		/* return to top of loop */
			}

			/*
			 * If we only need to join to the first matching inner tuple, then
			 * consider returning this one, but after that continue with next
			 * outer tuple.
			 */
			if (node->js.single_match)
				node->nl_NeedNewOuter = true;

			if (otherqual == NULL || ExecQual(otherqual, econtext))
			{
				/*
				 * qualification was satisfied so we project and return the
				 * slot containing the result tuple using ExecProject().
				 */
				ENL1_printf("qualification succeeded, projecting tuple");

				return ExecProject(node->js.ps.ps_ProjInfo);
			}
			else
				InstrCountFiltered2(node, 1);
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);

		ENL1_printf("qualification failed, looping");
	}
}

void InitHash(NestLoopState *node)
{
	EState *estate = node->js.ps.state;
	NestLoop *plan = (NestLoop*) node->js.ps.plan;
	Assert(IsBatched(plan));
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleDesc outer_tdesc = outerPlanState(node)->ps_ResultTupleDesc;
	// TODO figure out how to get all the join operators
	ListCell *lc;
	Oid *eqops = palloc(plan->hashOps->length * (sizeof(Oid)));
	int i = 0;
	
	node->numLookupAttrs = plan->hashOps->length;
	node->innerAttrs = palloc(node->numLookupAttrs * sizeof(AttrNumber));

	foreach(lc, plan->hashOps)
	{
		Oid eqop = lfirst_oid(lc);
		if (!OidIsValid(eqop))
			continue;
		eqops[i] = eqop;
		node->innerAttrs[i] = list_nth_int(plan->innerHashAttNos, i);
		i++;
	}
	Oid *eqFuncOids;
	execTuplesHashPrepare(i, eqops, &eqFuncOids, &node->hashFunctions);

	node->hashslot =
		ExecAllocTableSlot(&estate->es_tupleTable, outer_tdesc);
	int numattrs = plan->nestParams->length;
	AttrNumber *keyattrs = palloc(numattrs * (sizeof(AttrNumber)));
	i = 0;
	// TODO THIS IS WRONG
	foreach(lc, plan->nestParams)
	{
		NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);
		keyattrs[i++] = nlp->paramval->varattno;
	}

	// TODO make varname casing consistent
	// Size additional_size = sizeof(HashedTupleData);

	MemoryContext tablecxt =
		CreateThreadLocalCurrentMemoryContext(GetCurrentMemoryContext(), "NL_HASHTABLE");

	node->hashtable = BuildTupleHashTableExt(&node->js.ps, outer_tdesc, numattrs, keyattrs, eqFuncOids, node->hashFunctions, GetBatchSize(plan), 0, econtext->ecxt_per_query_memory, tablecxt, econtext->ecxt_per_tuple_memory, false);

	node->hashiterinit = false;
	node->current_hash_entry = NULL;
}

bool FlushTupleHash(NestLoopState *node, ExprContext *econtext)
{
	if (!node->hashiterinit)
	{
		InitTupleHashIterator(node->hashtable, &node->hashiter);
		node->hashiterinit = true;
		node->current_hash_entry = NULL;
	}
	
	
	TupleHashEntry entry = node->current_hash_entry;
	if (entry == NULL)
		entry = ScanTupleHashTable(node->hashtable, &node->hashiter);
	while (entry != NULL)
	{
		NLBucketInfo *binfo = entry->additional;
		if (!(binfo->matched))
		{
			if (binfo->current != NULL)
			{
				ExecStoreMinimalTuple(lfirst(binfo->current),
									  econtext->ecxt_outertuple,
									  false);
				binfo->current = binfo->current->next;
				node->current_hash_entry = entry;
				return true;
			}
		}
		entry = ScanTupleHashTable(node->hashtable, &node->hashiter);
	}
	TermTupleHashIterator(&node->hashiter);
	node->hashiterinit = false;
	node->current_hash_entry = NULL;
	return false;
}

bool GetNewOuterTupleHash(NestLoopState *node, ExprContext *econtext)
{
	TupleTableSlot *inner = econtext->ecxt_innertuple;
	TupleHashTable ht = node->hashtable;
	ExprState *eq = node->js.joinqual;

	TupleHashEntry data;
	data = FindTupleHashEntry(ht, inner, eq, node->hashFunctions, node->numLookupAttrs, node->innerAttrs);
	Assert(data != NULL);

	NLBucketInfo *binfo = (NLBucketInfo*) data->additional;
	if (binfo->current == NULL)
	{
		binfo->current = list_head(binfo->tuples);
		return false;
	}

	MinimalTuple mintp = (MinimalTuple ) lfirst(binfo->current);
	ExecStoreMinimalTuple(mintp, econtext->ecxt_outertuple, false);
	binfo->current = binfo->current->next;

	Assert(data != NULL);
	node->nl_MatchedOuter = false;
	return true;
}

void ResetBatchHash(NestLoopState *node, ExprContext *econtext)
{
	if (node->hashiterinit)
	{
		node->hashiterinit = false;
		TermTupleHashIterator(&node->hashiterinit);
	}
}

void RegisterOuterMatchHash(NestLoopState *node, ExprContext *econtext)
{
	TupleHashTable ht = node->hashtable;
	TupleHashEntry data = LookupTupleHashEntry(ht, econtext->ecxt_outertuple, NULL);
	Assert(data != NULL);
	((NLBucketInfo*)data->additional)->matched = true;
}

void AddTupleToOuterBatchHash(NestLoopState *node, TupleTableSlot *slot)
{
	TupleHashTable ht = node->hashtable;
	bool isnew = false;

	Assert(!TupIsNull(slot));
	TupleHashEntry orig_data = LookupTupleHashEntry(ht, slot, &isnew);
	Assert(orig_data != NULL);
	Assert(orig_data->firstTuple != NULL);
	MemoryContext cxt = MemoryContextSwitchTo(ht->tablecxt);
	MinimalTuple tuple;
	if (isnew)
	{
		orig_data->additional = palloc0(sizeof(NLBucketInfo));
		tuple = orig_data->firstTuple;
	}
	NLBucketInfo *binfo = (NLBucketInfo *) orig_data->additional;
	List *tl = binfo->tuples;
	if (!isnew)
	{
		tuple = ExecCopySlotMinimalTuple(slot);
	}

	binfo->tuples = list_append_unique_ptr(tl, tuple);
	binfo->current = list_head(binfo->tuples);
	MemoryContextSwitchTo(cxt);
}

void EndHash(NestLoopState *node)
{
	(void)node;
	MemoryContextDelete(node->hashtable->tablecxt);
	return;
}


void AddTupleToOuterBatch(NestLoopState *node, TupleTableSlot *slot)
{
	tuplestore_puttupleslot(node->batchedtuplestorestate,
							slot);
	node->nl_batchedmatchedinfo =
		lappend_int(node->nl_batchedmatchedinfo, 0);
}

void FreeBatchHash(NestLoopState *node)
{
	if (node->hashtable == NULL)
	{
		return;
	}
	node->hashiterinit = false;
	ResetTupleHashTable(node->hashtable);
	MemoryContextReset(node->hashtable->tablecxt);
	node->current_hash_entry = NULL;
}

bool UseHash(NestLoop *node, NestLoopState *nl)
{
	return node->hashOps != NIL && node->hashOps->length > 0;
}

bool CreateBatch(NestLoopState *node, ExprContext *econtext)
{
	bool outer_done = false;
	NestLoop   *nl = (NestLoop *) node->js.ps.plan;
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	LOCAL_JOIN_METHOD_0(FreeBatch, node);

	for (int batchno = 0; batchno < GetBatchSize(nl); batchno++)
	{
		ENL1_printf("getting new outer tuple");
		if (!outer_done)
		{
			outerTupleSlot = ExecProcNode(outerPlan);
		}

		/*
		* if there are no more outer tuples, then the join is complete..
		*/
		if (outer_done || TupIsNull(outerTupleSlot))
		{
			if (batchno == 0)
			{
				ENL1_printf("no outer tuple, ending join");
				return false;
			}
			else
			{
				outer_done = true;
			}
		}
		else
		{
			ENL1_printf("saving new outer tuple information");
			econtext->ecxt_outertuple = outerTupleSlot;
			if (IsBatched(nl))
			{
				LOCAL_JOIN_METHOD(AddTupleToOuterBatch, node, outerTupleSlot);
			}
		}

		/*
		* fetch the values of any outer Vars that must be passed to the
		* inner scan, and store them in the appropriate PARAM_EXEC slots.
		*/
		ListCell *lc;
		foreach(lc, nl->nestParams)
		{
			NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);
			int			paramno;
			if (!IsBatched(nl))
			{
				paramno = nlp->paramno;
			}
			else
			{
				paramno = 
					list_nth_int(nlp->batchedparams, batchno);
			}
			ParamExecData *prm;

			prm = &(econtext->ecxt_param_exec_vals[paramno]);
			/* Param value should be an OUTER_VAR var */
			Assert(IsA(nlp->paramval, Var));
			Assert(nlp->paramval->varno == OUTER_VAR);
			Assert(nlp->paramval->varattno > 0);
			if (!outer_done)
			{
				prm->value = slot_getattr(outerTupleSlot,
											nlp->paramval->varattno,
											&(prm->isnull));
			}
			else
			{
				prm->isnull = true;
			}
			/* Flag parameter value as changed */
			innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
												 paramno);
		}
	}

	if (IsBatched(nl))
	{
		LOCAL_JOIN_METHOD(ResetBatch, node, econtext);
	}

	return true;
}

bool FlushTuple(NestLoopState *node, ExprContext *econtext)
{
	if (node->batchedtuplestorestate == NULL)
	{
		return false;
	}
	while (node->nl_batchtupno < tuplestore_tuple_count(node->batchedtuplestorestate))
	{
		ListCell *lc = list_nth_cell(node->nl_batchedmatchedinfo, 
								 	 node->nl_batchtupno);
		if (lc->data.int_value == 0)
		{
			GetNewOuterTuple(node, econtext);
			return true;
		}
		tuplestore_skiptuples(node->batchedtuplestorestate, 1, true);
		node->nl_batchtupno++;
	}
	return false;
}

void RegisterOuterMatch(NestLoopState *node, ExprContext *econtext)
{
	if (node->batchedtuplestorestate == NULL)
	{
		return;
	}
	(void) econtext;
	ListCell *lc = list_nth_cell(node->nl_batchedmatchedinfo, 
								 node->nl_batchtupno - 1);
	lc->data.int_value = 1;
	return;
}


int GetBatchSize(NestLoop *node)
{
	if (node->nestParams == NULL)
	{
		return 1;
	}

	NestLoopParam *nlp = (NestLoopParam *) lfirst(list_head(node->nestParams));
	return nlp->batchedparams ? nlp->batchedparams->length : 1;
}

bool IsBatched(NestLoop *node)
{
	return GetBatchSize(node) > 1;
}

bool GetNewOuterTuple(NestLoopState *node, ExprContext *econtext)
{
	Tuplestorestate *outertuples = node->batchedtuplestorestate;
	if (!tuplestore_ateof(outertuples)
		&& tuplestore_tuple_count(outertuples) > 0
		&& tuplestore_gettupleslot(outertuples,
								   true,
								   false,
								   econtext->ecxt_outertuple))
	{
		node->nl_batchtupno++;
		node->nl_MatchedOuter = false;
		return true;
	}
	return false;
}

void ResetBatch(NestLoopState *node, ExprContext *econtext)
{
	Tuplestorestate *outertuples = node->batchedtuplestorestate;
	if (!outertuples)
	{
		return;
	}
	tuplestore_rescan(outertuples);
	node->nl_batchtupno = 0;
}

void FreeBatch(NestLoopState *node)
{
	Tuplestorestate *outertuples = node->batchedtuplestorestate;
	if (!outertuples)
	{
		return;
	}

	tuplestore_clear(node->batchedtuplestorestate);
	list_free(node->nl_batchedmatchedinfo);
	node->nl_batchedmatchedinfo = NIL;
}

/* ----------------------------------------------------------------
 *		ExecInitNestLoop
 * ----------------------------------------------------------------
 */
NestLoopState *
ExecInitNestLoop(NestLoop *node, EState *estate, int eflags)
{
	NestLoopState *nlstate;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	NL1_printf("ExecInitNestLoop: %s\n",
			   "initializing node");

	/*
	 * create state structure
	 */
	nlstate = makeNode(NestLoopState);
	nlstate->js.ps.plan = (Plan *) node;
	nlstate->js.ps.state = estate;
	nlstate->js.ps.ExecProcNode = ExecNestLoop;
	nlstate->batchedtuplestorestate = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &nlstate->js.ps);

	/*
	 * initialize child nodes
	 *
	 * If we have no parameters to pass into the inner rel from the outer,
	 * tell the inner child that cheap rescans would be good.  If we do have
	 * such parameters, then there is no point in REWIND support at all in the
	 * inner child, because it will always be rescanned with fresh parameter
	 * values.
	 */
	outerPlanState(nlstate) = ExecInitNode(outerPlan(node), estate, eflags);
	if (node->nestParams == NIL)
		eflags |= EXEC_FLAG_REWIND;
	else
		eflags &= ~EXEC_FLAG_REWIND;
	innerPlanState(nlstate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTupleSlotTL(&nlstate->js.ps);
	ExecAssignProjectionInfo(&nlstate->js.ps, NULL);

	/*
	 * initialize child expressions
	 */
	nlstate->js.ps.qual =
		ExecInitQual(node->join.plan.qual, (PlanState *) nlstate);
	nlstate->js.jointype = node->join.jointype;
	nlstate->js.joinqual =
		ExecInitQual(node->join.joinqual, (PlanState *) nlstate);

	/*
	 * detect whether we need only consider the first matching inner tuple
	 */
	nlstate->js.single_match = (node->join.inner_unique ||
								node->join.jointype == JOIN_SEMI);

	/* set up null tuples for outer joins, if needed */
	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			break;
		case JOIN_LEFT:
		case JOIN_ANTI:
			nlstate->nl_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
									  ExecGetResultType(innerPlanState(nlstate)));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * finally, wipe the current outer tuple clean.
	 */
	nlstate->nl_NeedNewOuter = true;
	nlstate->nl_NeedNewInner = true;
	nlstate->nl_MatchedOuter = false;

	NL1_printf("ExecInitNestLoop: %s\n",
			   "node initialized");

	if (IsBatched(node))
	{
		nlstate->nl_currentstatus = NL_INIT;
		nlstate->nl_batchedmatchedinfo = NIL;
		nlstate->nl_batchtupno = 0;
		
		if (UseHash(node, nlstate))
		{
			InitHash(nlstate);

			nlstate->FlushTupleImpl = &FlushTupleHash;
			nlstate->GetNewOuterTupleImpl = &GetNewOuterTupleHash;
			nlstate->ResetBatchImpl = &ResetBatchHash;
			nlstate->RegisterOuterMatchImpl = &RegisterOuterMatchHash;
			nlstate->AddTupleToOuterBatchImpl = &AddTupleToOuterBatchHash;
			nlstate->FreeBatchImpl = &FreeBatchHash;
		}
		else
		{
			nlstate->batchedtuplestorestate =
				tuplestore_begin_heap(true, false, work_mem);
			nlstate->FlushTupleImpl = &FlushTuple;
			nlstate->GetNewOuterTupleImpl = &GetNewOuterTuple;
			nlstate->ResetBatchImpl = &ResetBatch;
			nlstate->RegisterOuterMatchImpl = &RegisterOuterMatch;
			nlstate->AddTupleToOuterBatchImpl = &AddTupleToOuterBatch;
			nlstate->FreeBatchImpl = &FreeBatch;
		}
	}
	else
	{
		nlstate->nl_currentstatus = NL_NOBATCH;
		nlstate->FlushTupleImpl = &FlushTuple;
		nlstate->GetNewOuterTupleImpl = &GetNewOuterTuple;
		nlstate->ResetBatchImpl = &ResetBatch;
		nlstate->RegisterOuterMatchImpl = &RegisterOuterMatch;
		nlstate->AddTupleToOuterBatchImpl = &AddTupleToOuterBatch;
		nlstate->FreeBatchImpl = &FreeBatch;
	}

	return nlstate;
}

/* ----------------------------------------------------------------
 *		ExecEndNestLoop
 *
 *		closes down scans and frees allocated storage
 * ----------------------------------------------------------------
 */
void
ExecEndNestLoop(NestLoopState *node)
{
	NL1_printf("ExecEndNestLoop: %s\n",
			   "ending node processing");

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);

	if (node->batchedtuplestorestate != NULL)
	{
		tuplestore_end(node->batchedtuplestorestate);
		list_free(node->nl_batchedmatchedinfo);
		node->nl_batchedmatchedinfo = NIL;
	}

	/*
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));

	NL1_printf("ExecEndNestLoop: %s\n",
			   "node processing ended");
}

/* ----------------------------------------------------------------
 *		ExecReScanNestLoop
 * ----------------------------------------------------------------
 */
void
ExecReScanNestLoop(NestLoopState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * If outerPlan->chgParam is not null then plan will be automatically
	 * re-scanned by first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
	
	if (IsBatched((NestLoop*) node->js.ps.plan))
	{
		LOCAL_JOIN_METHOD_0(FreeBatch, node);
		node->nl_currentstatus = NL_INIT;
	}

	/*
	 * innerPlan is re-scanned for each new outer tuple and MUST NOT be
	 * re-scanned from here or you'll get troubles from inner index scans when
	 * outer Vars are used as run-time keys...
	 */

	node->nl_NeedNewOuter = true;
	node->nl_NeedNewInner = true;
	node->nl_MatchedOuter = false;
}
