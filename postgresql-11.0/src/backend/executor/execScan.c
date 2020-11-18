/*-------------------------------------------------------------------------
 *
 * execScan.c
 *	  This code provides support for generalized relation scans. ExecScan
 *	  is passed a node and a pointer to a function to "do the right thing"
 *	  and return a tuple from the relation. ExecScan then does the tedious
 *	  stuff - checking the qualification and projecting the tuple
 *	  appropriately.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/migrate_schema.h"
#include "access/htup_details.h"

bool MigrateTuple(TupleTableSlot *slot, uint32 k1, uint32 k2, uint32 k3)
{
	if (slot->tts_tuple == NULL | slot->tts_tuple->t_len == 0)
	{
		return true;
	}

	LWLock *bitmapLock;
	// FIXME: k1: c_w_id = 50, k2: c_d_id = 10, k3: c_id = 3000
	// page size = 300
	const uint32 page = 1;
	uint32 eid = (k1 * 10 + k2) * (3000 / page) + (k3 - 1) / page;
	uint32 wordid 		= getwordid(eid);
	uint32 lockbitid 	= getlockbitid(eid);
	uint32 migratebitid = getmigratebitid(eid);

	// printf("blockId: %d, pageSize: %d, offset: %d\n", blockId, pagesize, offset);
	// printf("k1: %d, k2: %d, k3: %d\n", k1, k2, k3);
	// printf("eid: %d, wordid: %d, lockbitid: %d, migratebitid: %d\n", eid, wordid, lockbitid, migratebitid);

	// if size == 0, it's the 1st micro-transaction at udf
	long size = 0;
	if (migrateudf)
		size = hash_get_num_entries(TrackingTable);
	// printf("#### hash size: %d\n", size);

	if (list_member_int(InProgLocalList0, eid)) {
		return true;
	}

	if (list_member_int(InProgLocalList1, eid)) {
		if (migrateudf)
			trackinghashtable_insert(TrackingTable, eid, 1);
		return false;
	}

	if (!getkthbit(PartialBitmap[wordid], migratebitid))
	{
		if (getkthbit(PartialBitmap[wordid], lockbitid))
		{
			InProgLocalList1 = pg_lappend_int(InProgLocalList1, eid);
			return false;
		}

		bitmapLock = MigrateBitmapPartitionLock(eid, BitmapNum);
		LWLockAcquire(bitmapLock, LW_EXCLUSIVE);

		if (!getkthbit(PartialBitmap[wordid], migratebitid))
		{
			if (!getkthbit(PartialBitmap[wordid], lockbitid))
			{
				PartialBitmap[wordid] |= ((uint64)1 << lockbitid);
				LWLockRelease(bitmapLock);

				InProgLocalList0 = pg_lappend_int(InProgLocalList0, eid);
				return true;
			}
			else
			{
				LWLockRelease(bitmapLock);

				InProgLocalList1 = pg_lappend_int(InProgLocalList1, eid);	
				return false;
			}
		}
		else
		{
			LWLockRelease(bitmapLock);
			if (size != 0) {
				trackinghashtable_delete(TrackingTable, eid);
			}
		}
	} else {
		if (size != 0) {
			trackinghashtable_delete(TrackingTable, eid);
		}
	}
	return false;
}

/*
 * ExecScanFetch -- check interrupts & fetch next potential tuple
 *
 * This routine is concerned with substituting a test tuple if we are
 * inside an EvalPlanQual recheck.  If we aren't, just execute
 * the access method's next-tuple routine.
 */
static inline TupleTableSlot *
ExecScanFetch(ScanState *node,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd)
{
	EState	   *estate = node->ps.state;

	CHECK_FOR_INTERRUPTS();

	if (estate->es_epqTuple != NULL)
	{
		/*
		 * We are inside an EvalPlanQual recheck.  Return the test tuple if
		 * one is available, after rechecking any access-method-specific
		 * conditions.
		 */
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid == 0)
		{
			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/*
			 * This is a ForeignScan or CustomScan which has pushed down a
			 * join to the remote side.  The recheck method is responsible not
			 * only for rechecking the scan/join quals but also for storing
			 * the correct tuple in the slot.
			 */
			if (!(*recheckMtd) (node, slot))
				ExecClearTuple(slot);	/* would not be returned by scan */
			return slot;
		}
		else if (estate->es_epqTupleSet[scanrelid - 1])
		{
			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Return empty slot if we already returned a tuple */
			if (estate->es_epqScanDone[scanrelid - 1])
				return ExecClearTuple(slot);
			/* Else mark to remember that we shouldn't return more */
			estate->es_epqScanDone[scanrelid - 1] = true;

			/* Return empty slot if we haven't got a test tuple */
			if (estate->es_epqTuple[scanrelid - 1] == NULL)
				return ExecClearTuple(slot);

			/* Store test tuple in the plan node's scan slot */
			ExecStoreTuple(estate->es_epqTuple[scanrelid - 1],
						   slot, InvalidBuffer, false);

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				ExecClearTuple(slot);	/* would not be returned by scan */

			return slot;
		}
	}

	/*
	 * Run the node-type-specific access method function to get the next tuple
	 */
	return (*accessMtd) (node);
}

/* ----------------------------------------------------------------
 *		ExecScan
 *
 *		Scans the relation using the 'access method' indicated and
 *		returns the next qualifying tuple in the direction specified
 *		in the global variable ExecDirection.
 *		The access method returns the next tuple and ExecScan() is
 *		responsible for checking the tuple returned against the qual-clause.
 *
 *		A 'recheck method' must also be provided that can check an
 *		arbitrary tuple of the relation against any qual conditions
 *		that are implemented internal to the access method.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecScan(ScanState *node,
		 ExecScanAccessMtd accessMtd,	/* function returning a tuple */
		 ExecScanRecheckMtd recheckMtd)
{
	ExprContext *econtext;
	ExprState  *qual;
	ProjectionInfo *projInfo;

	/*
	 * Fetch data from node
	 */
	qual = node->ps.qual;
	projInfo = node->ps.ps_ProjInfo;
	econtext = node->ps.ps_ExprContext;

	/* interrupt checks are in ExecScanFetch */

	/*
	 * If we have neither a qual to check nor a projection to do, just skip
	 * all the overhead and return the raw scan tuple.
	 */
	if (!qual && !projInfo)
	{
		TupleTableSlot *slot;

		ResetExprContext(econtext);
		slot = ExecScanFetch(node, accessMtd, recheckMtd);


		if (migrateflag)
		{
			// Get query's predicates
			bool k1isNull, k2isNull, k3isNull;
			Datum  d1 = heap_getattr(slot->tts_tuple, 1,
									 slot->tts_tupleDescriptor, &k1isNull);
			Datum  d2 = heap_getattr(slot->tts_tuple, 2,
									 slot->tts_tupleDescriptor, &k2isNull);
			Datum  d3 = heap_getattr(slot->tts_tuple, 3,
									 slot->tts_tupleDescriptor, &k3isNull);
			uint32 t1 = DatumGetUInt32(d1);
			uint32 t2 = DatumGetUInt32(d2);
			uint32 t3 = DatumGetUInt32(d3);
			if (MigrateTuple(slot, t1, t2, t3))
			{
				++tuplemigratecount;
				return slot;
			}
		}
		else
		{
			return slot;
		}
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * get a tuple from the access method.  Loop until we obtain a tuple that
	 * passes the qualification.
	 */
	for (;;)
	{
		TupleTableSlot *slot;

		slot = ExecScanFetch(node, accessMtd, recheckMtd);

		/*
		 * if the slot returned by the accessMtd contains NULL, then it means
		 * there is nothing more to scan so we just return an empty slot,
		 * being careful to use the projection result slot so it has correct
		 * tupleDesc.
		 */
		if (TupIsNull(slot))
		{
			if (projInfo)
				return ExecClearTuple(projInfo->pi_state.resultslot);
			else
				return slot;
		}

		/*
		 * place the current tuple into the expr context
		 */
		econtext->ecxt_scantuple = slot;

		/*
		 * check that the current tuple satisfies the qual-clause
		 *
		 * check for non-null qual here to avoid a function call to ExecQual()
		 * when the qual is null ... saves only a few cycles, but they add up
		 * ...
		 */
		if (qual == NULL || ExecQual(qual, econtext))
		{
			if (migrateflag)
			{
				// Get query's predicates
				bool k1isNull, k2isNull, k3isNull;
				Datum  d1 = heap_getattr(slot->tts_tuple, 1,
										slot->tts_tupleDescriptor, &k1isNull);
				Datum  d2 = heap_getattr(slot->tts_tuple, 2,
										slot->tts_tupleDescriptor, &k2isNull);
				Datum  d3 = heap_getattr(slot->tts_tuple, 3,
										slot->tts_tupleDescriptor, &k3isNull);
				uint32 t1 = DatumGetUInt32(d1);
				uint32 t2 = DatumGetUInt32(d2);
				uint32 t3 = DatumGetUInt32(d3);
				if (MigrateTuple(slot, t1, t2, t3))
				{
					++tuplemigratecount;

					if (projInfo)
						return ExecProject(projInfo);
					else
						return slot;
				}
			}
			else
			{
				/*
				 * Found a satisfactory scan tuple.
				 */
				if (projInfo)
				{
					/*
					 * Form a projection tuple, store it in the result tuple slot
					 * and return it.
					 */
					return ExecProject(projInfo);
				}
				else
				{
					/*
					 * Here, we aren't projecting, so just return scan tuple.
					 */
					return slot;
				}
			}
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);
	}
}

/*
 * ExecAssignScanProjectionInfo
 *		Set up projection info for a scan node, if necessary.
 *
 * We can avoid a projection step if the requested tlist exactly matches
 * the underlying tuple type.  If so, we just set ps_ProjInfo to NULL.
 * Note that this case occurs not only for simple "SELECT * FROM ...", but
 * also in most cases where there are joins or other processing nodes above
 * the scan node, because the planner will preferentially generate a matching
 * tlist.
 *
 * The scan slot's descriptor must have been set already.
 */
void
ExecAssignScanProjectionInfo(ScanState *node)
{
	Scan	   *scan = (Scan *) node->ps.plan;
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, scan->scanrelid);
}

/*
 * ExecAssignScanProjectionInfoWithVarno
 *		As above, but caller can specify varno expected in Vars in the tlist.
 */
void
ExecAssignScanProjectionInfoWithVarno(ScanState *node, Index varno)
{
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, varno);
}

/*
 * ExecScanReScan
 *
 * This must be called within the ReScan function of any plan node type
 * that uses ExecScan().
 */
void
ExecScanReScan(ScanState *node)
{
	EState	   *estate = node->ps.state;

	/*
	 * We must clear the scan tuple so that observers (e.g., execCurrent.c)
	 * can tell that this plan node is not positioned on a tuple.
	 */
	ExecClearTuple(node->ss_ScanTupleSlot);

	/* Rescan EvalPlanQual tuple if we're inside an EvalPlanQual recheck */
	if (estate->es_epqScanDone != NULL)
	{
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid > 0)
			estate->es_epqScanDone[scanrelid - 1] = false;
		else
		{
			Bitmapset  *relids;
			int			rtindex = -1;

			/*
			 * If an FDW or custom scan provider has replaced the join with a
			 * scan, there are multiple RTIs; reset the epqScanDone flag for
			 * all of them.
			 */
			if (IsA(node->ps.plan, ForeignScan))
				relids = ((ForeignScan *) node->ps.plan)->fs_relids;
			else if (IsA(node->ps.plan, CustomScan))
				relids = ((CustomScan *) node->ps.plan)->custom_relids;
			else
				elog(ERROR, "unexpected scan node: %d",
					 (int) nodeTag(node->ps.plan));

			while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
			{
				Assert(rtindex > 0);
				estate->es_epqScanDone[rtindex - 1] = false;
			}
		}
	}
}
