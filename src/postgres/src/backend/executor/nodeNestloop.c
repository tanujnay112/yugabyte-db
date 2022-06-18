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
#include "executor/nodeNestloop.h"
#include "miscadmin.h"
#include "utils/memutils.h"

bool CreateBatch(NestLoopState *node, ExprContext *econtext);
bool FlushTuple(NestLoopState *node, ExprContext *econtext);
int GetBatchSize(NestLoop *node);
bool IsBatched(NestLoop *node);
bool GetNewOuterTuple(NestLoopState *node, ExprContext *econtext);
void ResetBatch(NestLoopState *node, ExprContext *econtext);

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
	innerPlan = innerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;

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

		if (node->nl_currentstatus == NL_FLUSHING)
		{
			bool success = FlushTuple(node, econtext);
			if (success)
			{
				node->nl_NeedNewOuter = false;
				node->nl_NeedNewInner = false;
				goto innerscan;
			}
			/* tuplestate should be clean */
			node->nl_currentstatus = NL_INIT;
		}
		/*
		 * If we don't have an outer tuple, get the next one and reset the
		 * inner scan.
		 */
		if (node->nl_NeedNewOuter)
		{
			if (node->nl_currentstatus == NL_BATCHING)
			{
				Tuplestorestate *outertuples = node->batchedtuplestorestate;
				if (outertuples)
				{
					if(!GetNewOuterTuple(node, econtext))
					{
						node->nl_NeedNewInner = true;
						ResetBatch(node, econtext);
					}
					else
					{
						node->nl_NeedNewInner = false;
					}
					goto innerscan;
				}
			}
			else
			{
				Assert(node->nl_currentstatus == NL_INIT);
				node->nl_currentstatus = NL_BATCHING;
			}

			/* create batch */
			Assert(node->nl_currentstatus == NL_BATCHING || !IsBatched(nl));
			bool success = CreateBatch(node, econtext);
			if (!success)
				return NULL;
			/*
			* now rescan the inner plan
			*/
			ENL1_printf("rescanning inner plan");
			ExecReScan(innerPlan);
		}

	innerscan:
		/*
		 * we have an outerTuple, try to get the next inner tuple.
		 */
		ENL1_printf("getting new inner tuple");

		// Tuplestorestate *outertuples = node->batchedtuplestorestate;
		if (node->nl_NeedNewInner)
		{
			innerTupleSlot = ExecProcNode(innerPlan);
			econtext->ecxt_innertuple = innerTupleSlot;
		}
		innerTupleSlot = econtext->ecxt_innertuple;
		node->nl_NeedNewInner = false;

		if (TupIsNull(innerTupleSlot))
		{
			ENL1_printf("no inner tuple, need new outer tuple");

			node->nl_NeedNewOuter = true;
			node->nl_NeedNewInner = true;
			
			if (node->nl_currentstatus != NL_FLUSHING)
			{
				node->nl_currentstatus = NL_FLUSHING;
				continue;
			}

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
			node->nl_MatchedOuter = true;

			/* In an antijoin, we never return a matched tuple */
			if (node->js.jointype == JOIN_ANTI)
			{
				node->nl_NeedNewOuter = true;
				continue;		/* return to top of loop */
			}

			/*
			 * If we only need to join to the first matching inner tuple, then
			 * consider returning this one, but after that continue with next
			 * outer tuple.
			 */
			if (node->js.single_match)
			{
				node->nl_NeedNewOuter = true;
				node->nl_NeedNewInner = true;
			}

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

		if (IsBatched(nl))
		{
			node->nl_NeedNewOuter = true;
		}
		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);

		ENL1_printf("qualification failed, looping");
	}
}

bool CreateBatch(NestLoopState *node, ExprContext *econtext)
{
	bool outer_done = false;
	NestLoop   *nl = (NestLoop *) node->js.ps.plan;
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
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
				// TODO Just fill in the rest of the params with NULL
			}
		}
		else
		{
			ENL1_printf("saving new outer tuple information");
			econtext->ecxt_outertuple = outerTupleSlot;
			if (IsBatched(nl))
			{
				tuplestore_puttupleslot(node->batchedtuplestorestate,
										outerTupleSlot);
			}
		}

		// if (outer_done)
		// {
		// 	tuplestore_gettupleslot(node->batchedtuplestorestate,
		// 							true, false, outerTupleSlot);
		// 	econtext->ecxt_outertuple = outerTupleSlot;
		// }

		node->nl_NeedNewOuter = false;
		node->nl_MatchedOuter = false;

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
		tuplestore_rescan(node->batchedtuplestorestate);
		tuplestore_gettupleslot(node->batchedtuplestorestate, true, false, econtext->ecxt_outertuple);
	}

	return true;
}

bool FlushTuple(NestLoopState *node, ExprContext *econtext)
{
	tuplestore_clear(node->batchedtuplestorestate);
	return false;
}

int GetBatchSize(NestLoop *node)
{
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
		return true;
	}
	return false;
}

void ResetBatch(NestLoopState *node, ExprContext *econtext)
{
	Tuplestorestate *outertuples = node->batchedtuplestorestate;
	tuplestore_rescan(outertuples);
	tuplestore_gettupleslot(outertuples, true, false,
							econtext->ecxt_outertuple);
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

	if (IsBatched(node))
	{
		nlstate->batchedtuplestorestate =
			tuplestore_begin_heap(true, false, work_mem);
		nlstate->nl_currentstatus = NL_INIT;
	}

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

	/*
	 * innerPlan is re-scanned for each new outer tuple and MUST NOT be
	 * re-scanned from here or you'll get troubles from inner index scans when
	 * outer Vars are used as run-time keys...
	 */

	node->nl_NeedNewOuter = true;
	node->nl_NeedNewInner = true;
	node->nl_MatchedOuter = false;
}
