/* -------------------------------------------------------------------------
 *
 * pquery.cpp
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/gausskernel/process/tcop/pquery.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/xact.h"
#include "commands/prepare.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "pg_trace.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#include "optimizer/pgxcplan.h"
#include "pgxc/execRemote.h"
#include "access/relscan.h"
#endif
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "workload/cpwlm.h"
#include "workload/workload.h"
#include "workload/commgr.h"
#include "pgstat.h"
#include "access/printtup.h"
#include "access/tableam.h"
#include "instruments/instr_unique_sql.h"
#include "executor/lightProxy.h"
#include "gstrace/gstrace_infra.h"
#include "gstrace/tcop_gstrace.h"
#include "storage/mot/jit_exec.h"

/*
 * ActivePortal is the currently executing Portal (the most closely nested,
 * if there are several).
 */
THR_LOCAL Portal ActivePortal = NULL;

static void process_query(
    PlannedStmt* plan, const char* source_text, ParamListInfo params, bool isMMTable,
    JitExec::JitContext* mot_jit_context, DestReceiver* dest, char* completion_tag);
static void fill_portal_store(Portal portal, bool is_top_level);
static uint32 run_from_store(Portal portal, ScanDirection direction, long count, DestReceiver* dest);
static uint32 run_from_explain_store(Portal portal, ScanDirection direction, DestReceiver* dest);

static uint64 portal_run_select(Portal portal, bool forward, long count, DestReceiver* dest);
static void portal_run_utility(
    Portal portal, Node* utility_stmt, bool is_top_level, DestReceiver* dest, char* completion_tag);
static void portal_run_multi(
    Portal portal, bool is_top_level, DestReceiver* dest, DestReceiver* altdest, char* completion_tag);
static long do_portal_run_fetch(Portal portal, FetchDirection fdirection, long count, DestReceiver* dest);
static void do_portal_rewind(Portal portal);

extern bool StreamTopConsumerAmI();

extern void report_qps_type(CmdType command_type);
extern CmdType set_cmd_type(const char* command_tag);
/*
 * CreateQueryDesc
 */
QueryDesc* CreateQueryDesc(PlannedStmt* plannedstmt, const char* source_text, Snapshot snapshot,
    Snapshot crosscheck_snapshot, DestReceiver* dest, ParamListInfo params, int instrument_options)
{
    QueryDesc* qd = (QueryDesc*)palloc(sizeof(QueryDesc));

    qd->operation = plannedstmt->commandType;   /* operation */
    qd->plannedstmt = plannedstmt;              /* plan */
    qd->utilitystmt = plannedstmt->utilityStmt; /* in case DECLARE CURSOR */
    qd->sourceText = source_text;               /* query text */
    qd->snapshot = RegisterSnapshot(snapshot);  /* snapshot */
    /* RI check snapshot */
    qd->crosscheck_snapshot = RegisterSnapshot(crosscheck_snapshot);
    qd->dest = dest;     /* output dest */
    qd->params = params; /* parameter values passed into query */

    if (IS_PGXC_DATANODE && plannedstmt->instrument_option) {
        qd->instrument_options = plannedstmt->instrument_option;
    } else {
        qd->instrument_options = instrument_options; /* instrumentation wanted? */
    }
    /* null these fields until set by ExecutorStart */
    qd->tupDesc = NULL;
    qd->estate = NULL;
    qd->planstate = NULL;
    qd->totaltime = NULL;
    qd->executed = false;
    qd->mot_jit_context = NULL;

    return qd;
}

/*
 * CreateUtilityQueryDesc
 */
QueryDesc* CreateUtilityQueryDesc(
    Node* utility_stmt, const char* source_text, Snapshot snapshot, DestReceiver* dest, ParamListInfo params)
{
    QueryDesc* qd = (QueryDesc*)palloc(sizeof(QueryDesc));

    qd->operation = CMD_UTILITY; /* operation */
    qd->plannedstmt = NULL;
    qd->utilitystmt = utility_stmt;            /* utility command */
    qd->sourceText = source_text;              /* query text */
    qd->snapshot = RegisterSnapshot(snapshot); /* snapshot */
    qd->crosscheck_snapshot = InvalidSnapshot; /* RI check snapshot */
    qd->dest = dest;                           /* output dest */
    qd->params = params;                       /* parameter values passed into query */
    qd->instrument_options = false;            /* uninteresting for utilities */

    /* null these fields until set by ExecutorStart */
    qd->tupDesc = NULL;
    qd->estate = NULL;
    qd->planstate = NULL;
    qd->totaltime = NULL;
    qd->executed = false;

    return qd;
}

/*
 * FreeQueryDesc
 */
void FreeQueryDesc(QueryDesc* qdesc)
{
    /* Can't be a live query */
    AssertEreport(qdesc->estate == NULL, MOD_EXECUTOR, "query is still living");

    /* forget our snapshots */
    UnregisterSnapshot(qdesc->snapshot);
    UnregisterSnapshot(qdesc->crosscheck_snapshot);

    /* Only the QueryDesc itself need be freed */
    pfree_ext(qdesc);
}

/*
 * ProcessQuery
 *		Execute a single plannable query within a PORTAL_MULTI_QUERY,
 *		PORTAL_ONE_RETURNING, or PORTAL_ONE_MOD_WITH portal
 *
 *	plan: the plan tree for the query
 *	source_text: the source text of the query
 *	params: any parameters needed
 *	dest: where to send results
 *	completion_tag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completion_tag may be NULL if caller doesn't want a status string.
 *
 * Must be called in a memory context that will be reset or deleted on
 * error; otherwise the executor's memory usage will be leaked.
 */
static void process_query(
    PlannedStmt* plan, const char* source_text, ParamListInfo params, bool isMMTable,
    JitExec::JitContext* mot_jit_context,DestReceiver* dest, char* completion_tag)
{
    QueryDesc* query_desc = NULL;
    Snapshot snap = NULL;

    elog(DEBUG3, "ProcessQuery");

    /******************************* MOT LLVM *************************************/
    if (isMMTable && !IS_PGXC_COORDINATOR && JitExec::IsMotCodegenEnabled() && mot_jit_context) {
        Oid lastOid = InvalidOid;
        uint64 tp_processed = 0;
        int scan_ended = 0;
        if (JitExec::IsMotCodegenPrintEnabled()) {
            elog(DEBUG1, "Invoking jitted mot query and query string: %s\n", source_text);
        }
        int rc = JitExec::JitExecQuery(mot_jit_context, params, NULL, &tp_processed, &scan_ended);
        if (JitExec::IsMotCodegenPrintEnabled()) {
            elog(DEBUG1, "jitted mot query returned: %d\n", rc);
        }

        if (completion_tag != NULL) {
            errno_t ret = EOK;

            switch (plan->commandType) {
                case CMD_INSERT:
                    ret = snprintf_s(completion_tag, COMPLETION_TAG_BUFSIZE, COMPLETION_TAG_BUFSIZE - 1,
                            "INSERT %u %lu", lastOid, tp_processed);
                    securec_check_ss(ret,"\0","\0");
                    break;
                case CMD_UPDATE:
                    ret = snprintf_s(completion_tag, COMPLETION_TAG_BUFSIZE, COMPLETION_TAG_BUFSIZE - 1,
                            "UPDATE %lu", tp_processed);
                    securec_check_ss(ret,"\0","\0");
                    break;
                case CMD_DELETE:
                    ret = snprintf_s(completion_tag, COMPLETION_TAG_BUFSIZE, COMPLETION_TAG_BUFSIZE - 1,
                            "DELETE %lu", tp_processed);
                    securec_check_ss(ret,"\0","\0");
                    break;
                default:
                    break;
            }
        }
        return;
    }
    /******************************* MOT LLVM *************************************/

    if (!isMMTable) {
        snap = GetActiveSnapshot();
    }

    /*
     * Create the QueryDesc object
     */
    query_desc = CreateQueryDesc(plan, source_text, snap, InvalidSnapshot, dest, params, 0);

    // save MOT JIT context
    query_desc->mot_jit_context = mot_jit_context;

    if (ENABLE_WORKLOAD_CONTROL && (IS_PGXC_COORDINATOR || IS_SINGLE_NODE)) {
        WLMTopSQLReady(query_desc);
    }

    if (IS_PGXC_COORDINATOR || IS_SINGLE_NODE) {
        if (u_sess->exec_cxt.need_track_resource) {
            query_desc->instrument_options = plan->instrument_option;
            query_desc->plannedstmt->instrument_option = plan->instrument_option;
        } else {
            query_desc->plannedstmt->instrument_option = 0;
        }
    }

    /*
     * Call ExecutorStart to prepare the plan for execution
     */
    ExecutorStart(query_desc, 0);

    /* Pass row trigger shippability info to estate */
    query_desc->estate->isRowTriggerShippable = plan->isRowTriggerShippable;

    /* workload client manager */
    if (ENABLE_WORKLOAD_CONTROL) {
        WLMInitQueryPlan(query_desc);
        dywlm_client_manager(query_desc);
    }

    /*
     * Run the plan to completion.
     */
    ExecutorRun(query_desc, ForwardScanDirection, 0L);

    /*
     * Build command completion status string, if caller wants one.
     */
    if (completion_tag != NULL) {
        Oid last_oid;
        errno_t ret = EOK;

        switch (query_desc->operation) {
            case CMD_SELECT:
                ret = snprintf_s(completion_tag,
                    COMPLETION_TAG_BUFSIZE,
                    COMPLETION_TAG_BUFSIZE - 1,
                    "SELECT %lu",
                    query_desc->estate->es_processed);
                securec_check_ss(ret, "\0", "\0");
                break;
            case CMD_INSERT:
                if (query_desc->estate->es_processed == 1)
                    last_oid = query_desc->estate->es_lastoid;
                else
                    last_oid = InvalidOid;
                ret = snprintf_s(completion_tag,
                    COMPLETION_TAG_BUFSIZE,
                    COMPLETION_TAG_BUFSIZE - 1,
                    "INSERT %u %lu",
                    last_oid,
                    query_desc->estate->es_processed);
                securec_check_ss(ret, "\0", "\0");
                break;
            case CMD_UPDATE:
                ret = snprintf_s(completion_tag,
                    COMPLETION_TAG_BUFSIZE,
                    COMPLETION_TAG_BUFSIZE - 1,
                    "UPDATE %lu",
                    query_desc->estate->es_processed);
                securec_check_ss(ret, "\0", "\0");
                break;
            case CMD_DELETE:
                ret = snprintf_s(completion_tag,
                    COMPLETION_TAG_BUFSIZE,
                    COMPLETION_TAG_BUFSIZE - 1,
                    "DELETE %lu",
                    query_desc->estate->es_processed);
                securec_check_ss(ret, "\0", "\0");
                break;
            case CMD_MERGE:
                ret = snprintf_s(completion_tag,
                    COMPLETION_TAG_BUFSIZE,
                    COMPLETION_TAG_BUFSIZE - 1,
                    "MERGE %lu",
                    query_desc->estate->es_processed);
                securec_check_ss(ret, "\0", "\0");
                break;
            default:
                ret = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE, "?\?\?");
                securec_check(ret, "\0", "\0");
                break;
        }
    }

    /*
     * Now, we close down all the scans and free allocated resources.
     */
    ExecutorFinish(query_desc);
    ExecutorEnd(query_desc);

    FreeQueryDesc(query_desc);
}

/*
 * ChoosePortalStrategy
 *		Select portal execution strategy given the intended statement list.
 *
 * The list elements can be Querys, PlannedStmts, or utility statements.
 * That's more general than portals need, but plancache.c uses this too.
 *
 * See the comments in portal.h.
 */
PortalStrategy ChoosePortalStrategy(List* stmts)
{
    int set_tag;
    ListCell* lc = NULL;

    /*
     * PORTAL_ONE_SELECT and PORTAL_UTIL_SELECT need only consider the
     * single-statement case, since there are no rewrite rules that can add
     * auxiliary queries to a SELECT or a utility command. PORTAL_ONE_MOD_WITH
     * likewise allows only one top-level statement.
     */
    if (list_length(stmts) == 1) {
        Node* stmt = (Node*)linitial(stmts);

        if (IsA(stmt, Query)) {
            Query* query = (Query*)stmt;

            if (query->canSetTag) {
                if (query->commandType == CMD_SELECT && query->utilityStmt == NULL) {
                    if (query->hasModifyingCTE)
                        return PORTAL_ONE_MOD_WITH;
                    else
                        return PORTAL_ONE_SELECT;
                }
                if (query->commandType == CMD_UTILITY && query->utilityStmt != NULL) {
                    if (UtilityReturnsTuples(query->utilityStmt))
                        return PORTAL_UTIL_SELECT;
                    /* it can't be ONE_RETURNING, so give up */
                    return PORTAL_MULTI_QUERY;
                }
#ifdef PGXC
                /*
                 * This is possible with an EXECUTE DIRECT in a SPI.
                 * There might be a better way to manage the
                 * cases with EXECUTE DIRECT here like using a special
                 * utility command and redirect it to a correct portal
                 * strategy.
                 * Something like PORTAL_UTIL_SELECT might be far better.
                 */
                if (query->commandType == CMD_SELECT && query->utilityStmt != NULL &&
                    IsA(query->utilityStmt, RemoteQuery)) {
                    RemoteQuery* step = (RemoteQuery*)query->utilityStmt;
                    /*
                     * Let's choose PORTAL_ONE_SELECT for now
                     * After adding more PGXC functionality we may have more
                     * sophisticated algorithm of determining portal strategy
                     *
                     * EXECUTE DIRECT is a utility but depending on its inner query
                     * it can return tuples or not depending on the query used.
                     */
                    if (step->exec_direct_type == EXEC_DIRECT_SELECT || step->exec_direct_type == EXEC_DIRECT_UPDATE ||
                        step->exec_direct_type == EXEC_DIRECT_DELETE || step->exec_direct_type == EXEC_DIRECT_INSERT ||
                        step->exec_direct_type == EXEC_DIRECT_LOCAL)
                        return PORTAL_ONE_SELECT;
                    else if (step->exec_direct_type == EXEC_DIRECT_UTILITY ||
                             step->exec_direct_type == EXEC_DIRECT_LOCAL_UTILITY)
                        return PORTAL_MULTI_QUERY;
                    else
                        return PORTAL_ONE_SELECT;
                }
#endif
            }
        }
#ifdef PGXC
        else if (IsA(stmt, RemoteQuery)) {
            RemoteQuery* step = (RemoteQuery*)stmt;
            /*
             * Let's choose PORTAL_ONE_SELECT for now
             * After adding more PGXC functionality we may have more
             * sophisticated algorithm of determining portal strategy.
             *
             * EXECUTE DIRECT is a utility but depending on its inner query
             * it can return tuples or not depending on the query used.
             */
            if (step->exec_direct_type == EXEC_DIRECT_SELECT || step->exec_direct_type == EXEC_DIRECT_UPDATE ||
                step->exec_direct_type == EXEC_DIRECT_DELETE || step->exec_direct_type == EXEC_DIRECT_INSERT ||
                step->exec_direct_type == EXEC_DIRECT_LOCAL)
                return PORTAL_ONE_SELECT;
            else if (step->exec_direct_type == EXEC_DIRECT_UTILITY ||
                     step->exec_direct_type == EXEC_DIRECT_LOCAL_UTILITY)
                return PORTAL_MULTI_QUERY;
            else
                return PORTAL_ONE_SELECT;
        }
#endif
        else if (IsA(stmt, PlannedStmt)) {
            PlannedStmt* pstmt = (PlannedStmt*)stmt;

            if (pstmt->canSetTag) {
                if (pstmt->commandType == CMD_SELECT && pstmt->utilityStmt == NULL) {
                    if (pstmt->hasModifyingCTE)
                        return PORTAL_ONE_MOD_WITH;
                    else
                        return PORTAL_ONE_SELECT;
                }
            }
        } else {
            /* must be a utility command; assume it's canSetTag */
            if (UtilityReturnsTuples(stmt))
                return PORTAL_UTIL_SELECT;
            /* it can't be ONE_RETURNING, so give up */
            return PORTAL_MULTI_QUERY;
        }
    }

    /*
     * PORTAL_ONE_RETURNING has to allow auxiliary queries added by rewrite.
     * Choose PORTAL_ONE_RETURNING if there is exactly one canSetTag query and
     * it has a RETURNING list.
     */
    set_tag = 0;
    foreach (lc, stmts) {
        Node* stmt = (Node*)lfirst(lc);

        if (IsA(stmt, Query)) {
            Query* query = (Query*)stmt;

            if (query->canSetTag) {
                if (++set_tag > 1)
                    return PORTAL_MULTI_QUERY; /* no need to look further */
                if (query->returningList == NIL)
                    return PORTAL_MULTI_QUERY; /* no need to look further */
            }
        } else if (IsA(stmt, PlannedStmt)) {
            PlannedStmt* pstmt = (PlannedStmt*)stmt;

            if (pstmt->canSetTag) {
                if (++set_tag > 1)
                    return PORTAL_MULTI_QUERY; /* no need to look further */
                if (!pstmt->hasReturning)
                    return PORTAL_MULTI_QUERY; /* no need to look further */
            }
        }
        /* otherwise, utility command, assumed not canSetTag */
    }
    if (set_tag == 1)
        return PORTAL_ONE_RETURNING;

    /* Else, it's the general case... */
    return PORTAL_MULTI_QUERY;
}

/*
 * FetchPortalTargetList
 *		Given a portal that returns tuples, extract the query targetlist.
 *		Returns NIL if the portal doesn't have a determinable targetlist.
 *
 * Note: do not modify the result.
 */
List* FetchPortalTargetList(Portal portal)
{
    /* no point in looking if we determined it doesn't return tuples */
    if (portal->strategy == PORTAL_MULTI_QUERY)
        return NIL;
    /* get the primary statement and find out what it returns */
    return FetchStatementTargetList(PortalGetPrimaryStmt(portal));
}

/*
 * FetchStatementTargetList
 *		Given a statement that returns tuples, extract the query targetlist.
 *		Returns NIL if the statement doesn't have a determinable targetlist.
 *
 * This can be applied to a Query, a PlannedStmt, or a utility statement.
 * That's more general than portals need, but plancache.c uses this too.
 *
 * Note: do not modify the result.
 *
 * XXX be careful to keep this in sync with UtilityReturnsTuples.
 */
List* FetchStatementTargetList(Node* stmt)
{
    if (stmt == NULL)
        return NIL;
    if (IsA(stmt, Query)) {
        Query* query = (Query*)stmt;

        if (query->commandType == CMD_UTILITY && query->utilityStmt != NULL) {
            /* transfer attention to utility statement */
            stmt = query->utilityStmt;
        } else {
            if (query->commandType == CMD_SELECT && query->utilityStmt == NULL)
                return query->targetList;
            if (query->returningList)
                return query->returningList;
            return NIL;
        }
    }
    if (IsA(stmt, PlannedStmt)) {
        PlannedStmt* pstmt = (PlannedStmt*)stmt;

        if (pstmt->commandType == CMD_SELECT && pstmt->utilityStmt == NULL)
            return pstmt->planTree->targetlist;
        if (pstmt->hasReturning)
            return pstmt->planTree->targetlist;
        return NIL;
    }
    if (IsA(stmt, FetchStmt)) {
        FetchStmt* fstmt = (FetchStmt*)stmt;
        Portal subportal;

        AssertEreport(!fstmt->ismove, MOD_EXECUTOR, "FetchStmt ismove can not be true");
        subportal = GetPortalByName(fstmt->portalname);
        AssertEreport(PortalIsValid(subportal), MOD_EXECUTOR, "subportal not valid");
        return FetchPortalTargetList(subportal);
    }
    if (IsA(stmt, ExecuteStmt)) {
        ExecuteStmt* estmt = (ExecuteStmt*)stmt;
        PreparedStatement* entry = NULL;

        entry = FetchPreparedStatement(estmt->name, true);
        return FetchPreparedStatementTargetList(entry);
    }
    return NIL;
}

static bool IsSupportExplain(const char* command_tag)
{
    if (strncmp(command_tag, "SELECT", 7) == 0)
        return true;
    if (strncmp(command_tag, "INSERT", 7) == 0)
        return true;
    if (strncmp(command_tag, "DELETE", 7) == 0)
        return true;
    if (strncmp(command_tag, "UPDATE", 7) == 0)
        return true;
    if (strncmp(command_tag, "MERGE", 6) == 0)
        return true;
    if (strncmp(command_tag, "SELECT INTO", 12) == 0)
        return true;
    if (strncmp(command_tag, "CREATE TABLE AS", 16) == 0)
        return true;
    return false;
}

/*
 * PortalStart
 *		Prepare a portal for execution.
 *
 * Caller must already have created the portal, done PortalDefineQuery(),
 * and adjusted portal options if needed.
 *
 * If parameters are needed by the query, they must be passed in "params"
 * (caller is responsible for giving them appropriate lifetime).
 *
 * The caller can also provide an initial set of "eflags" to be passed to
 * ExecutorStart (but note these can be modified internally, and they are
 * currently only honored for PORTAL_ONE_SELECT portals).  Most callers
 * should simply pass zero.
 *
 * The caller can optionally pass a snapshot to be used; pass InvalidSnapshot
 * for the normal behavior of setting a new snapshot.  This parameter is
 * presently ignored for non-PORTAL_ONE_SELECT portals (it's only intended
 * to be used for cursors).
 *
 * On return, portal is ready to accept PortalRun() calls, and the result
 * tupdesc (if any) is known.
 */

bool shouldDoInstrument(Portal portal, PlannedStmt* ps)
{
    if (((IS_PGXC_COORDINATOR && ps->is_stream_plan == true && !u_sess->attr.attr_sql.enable_cluster_resize) ||
        IS_SINGLE_NODE) &&
        u_sess->attr.attr_resource.resource_track_level == RESOURCE_TRACK_OPERATOR &&
        u_sess->attr.attr_resource.use_workload_manager &&
        t_thrd.wlm_cxt.collect_info->status != WLM_STATUS_RUNNING &&
        !portal->visible &&
        IsSupportExplain(portal->commandTag)) {
        return true;
    }

    return false;
}

void PortalStart(Portal portal, ParamListInfo params, int eflags, Snapshot snapshot)
{
    gstrace_entry(GS_TRC_ID_PortalStart);
    Portal save_active_portal;
    ResourceOwner save_resource_owner;
    MemoryContext save_portal_context;
    MemoryContext old_context;
    QueryDesc* query_desc = NULL;
    int myeflags;
    PlannedStmt* ps = NULL;
    Snapshot snap = NULL;
    int instrument_option = 0;

    AssertArg(PortalIsValid(portal));
    AssertState(portal->status == PORTAL_DEFINED);

    /*
     * Set up global portal context pointers.
     */
    save_active_portal = ActivePortal;
    save_resource_owner = t_thrd.utils_cxt.CurrentResourceOwner;
    save_portal_context = t_thrd.mem_cxt.portal_mem_cxt;
    PG_TRY();
    {
        ActivePortal = portal;
        t_thrd.utils_cxt.CurrentResourceOwner = portal->resowner;
        t_thrd.mem_cxt.portal_mem_cxt = PortalGetHeapMemory(portal);

        old_context = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

        /* Must remember portal param list, if any */
        portal->portalParams = params;

        /*
         * Determine the portal execution strategy
         */
        portal->strategy = ChoosePortalStrategy(portal->stmts);

        // Allocate and initialize scan descriptor
        portal->scanDesc = (HeapScanDesc)palloc0(SizeofHeapScanDescData + MaxHeapTupleSize);

        /*
         * Fire her up according to the strategy
         */
        switch (portal->strategy) {
            case PORTAL_ONE_SELECT:
                ps = (PlannedStmt*)linitial(portal->stmts);

                /* Must set snapshot before starting executor, unless it is a query with only MM Tables. */
                if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                    if (snapshot) {
                        PushActiveSnapshot(snapshot);
                    } else {
                        if (u_sess->pgxc_cxt.gc_fdw_snapshot) {
                            PushActiveSnapshot(u_sess->pgxc_cxt.gc_fdw_snapshot);
                        } else {
                            bool force_local_snapshot = false;

                            if (portal->cplan != NULL && portal->cplan->single_shard_stmt) {
                                /* with single shard, we will be forced to do local snapshot work */
                                force_local_snapshot = true;
                            }
                            PushActiveSnapshot(GetTransactionSnapshot(force_local_snapshot));
                        }
                    }
                }

                /*
                 * For operator track of active SQL, explain performance is triggered for SELECT SQL,
                 * except cursor case(portal->visible), which can't be run out within one fetch stmt
                 */
                if (shouldDoInstrument(portal, ps)) {
                    instrument_option |= INSTRUMENT_TIMER;
                    instrument_option |= INSTRUMENT_BUFFERS;
                }

                if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                    snap = GetActiveSnapshot();
                }

                /*
                 * Create QueryDesc in portal's context; for the moment, set
                 * the destination to DestNone.
                 */
                query_desc = CreateQueryDesc(ps, portal->sourceText, snap, InvalidSnapshot, None_Receiver, params, 0);

                // save MOT JIT context
                if (portal->cplan != NULL) {
                    query_desc->mot_jit_context = portal->cplan->mot_jit_context;
                }

                /* means on CN of the compute pool. */
                if (((IS_PGXC_COORDINATOR && StreamTopConsumerAmI()) || IS_SINGLE_NODE) && ps->instrument_option) {
                    query_desc->instrument_options |= ps->instrument_option;
                }

                /* Check if need track resource */
                if (u_sess->attr.attr_resource.use_workload_manager && (IS_PGXC_COORDINATOR || IS_SINGLE_NODE))
                    u_sess->exec_cxt.need_track_resource = WLMNeedTrackResource(query_desc);

                if (IS_PGXC_COORDINATOR || IS_SINGLE_NODE) {
                    if (u_sess->exec_cxt.need_track_resource) {
                        query_desc->instrument_options |= instrument_option;
                        query_desc->plannedstmt->instrument_option = instrument_option;
                    }
                }

                if (!u_sess->instr_cxt.obs_instr && 
                    ((query_desc->plannedstmt) != NULL && query_desc->plannedstmt->has_obsrel)) {
                    AutoContextSwitch cxtGuard(u_sess->top_mem_cxt);

                    u_sess->instr_cxt.obs_instr = New(CurrentMemoryContext) OBSInstrumentation();
                }

                /*
                 * If it's a scrollable cursor, executor needs to support
                 * REWIND and backwards scan, as well as whatever the caller
                 * might've asked for.
                 */
                if (portal->cursorOptions & CURSOR_OPT_SCROLL)
                    myeflags = eflags | EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD;
                /* with hold cursor need rewind portal to store all tuples */
                else if (portal->cursorOptions & CURSOR_OPT_HOLD)
                    myeflags = eflags | EXEC_FLAG_REWIND;
                else
                    myeflags = eflags;

                if (ENABLE_WORKLOAD_CONTROL && IS_PGXC_DATANODE) {
                    WLMCreateDNodeInfoOnDN(query_desc);

                    // create IO info on DN
                    WLMCreateIOInfoOnDN();
                }

                /*
                 * Call ExecutorStart to prepare the plan for execution
                 */
                ExecutorStart(query_desc, myeflags);

                /*
                 * This tells PortalCleanup to shut down the executor
                 */
                portal->queryDesc = query_desc;

                /*
                 * Remember tuple descriptor (computed by ExecutorStart)
                 */
                portal->tupDesc = query_desc->tupDesc;

                /*
                 * Reset cursor position data to "start of query"
                 */
                portal->atStart = true;
                portal->atEnd = false; /* allow fetches */
                portal->portalPos = 0;
                portal->posOverflow = false;

                if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                    PopActiveSnapshot();
                }
                break;

            case PORTAL_ONE_RETURNING:
            case PORTAL_ONE_MOD_WITH:

                /*
                 * We don't start the executor until we are told to run the
                 * portal.	We do need to set up the result tupdesc.
                 */
                {
                    PlannedStmt* pstmt = NULL;

                    pstmt = (PlannedStmt*)PortalGetPrimaryStmt(portal);
                    AssertEreport(IsA(pstmt, PlannedStmt), MOD_EXECUTOR, "pstmt is not a PlannedStmt");
                    portal->tupDesc = ExecCleanTypeFromTL(pstmt->planTree->targetlist, false);
                }

                /*
                 * Reset cursor position data to "start of query"
                 */
                portal->atStart = true;
                portal->atEnd = false; /* allow fetches */
                portal->portalPos = 0;
                portal->posOverflow = false;
                break;

            case PORTAL_UTIL_SELECT:

                /*
                 * We don't set snapshot here, because portal_run_utility will
                 * take care of it if needed.
                 */
                {
                    Node* ustmt = PortalGetPrimaryStmt(portal);

                    AssertEreport(!IsA(ustmt, PlannedStmt), MOD_EXECUTOR, "ustmt can not be a PlannedStmt");
                    portal->tupDesc = UtilityTupleDescriptor(ustmt);
                }

                /*
                 * Reset cursor position data to "start of query"
                 */
                portal->atStart = true;
                portal->atEnd = false; /* allow fetches */
                portal->portalPos = 0;
                portal->posOverflow = false;
                break;

            case PORTAL_MULTI_QUERY:
                /* Need do nothing now */
                portal->tupDesc = NULL;

                if (ENABLE_WORKLOAD_CONTROL && IS_PGXC_DATANODE) {
                    WLMCreateDNodeInfoOnDN(NULL);

                    // create IO info on DN
                    WLMCreateIOInfoOnDN();
                }
                break;
            default:
                break;
        }

        portal->stmtMemCost = 0;
    }
    PG_CATCH();
    {
        /* Uncaught error while executing portal: mark it dead */
        MarkPortalFailed(portal);

        /* Restore global vars and propagate error */
        ActivePortal = save_active_portal;
        t_thrd.utils_cxt.CurrentResourceOwner = save_resource_owner;
        t_thrd.mem_cxt.portal_mem_cxt = save_portal_context;

        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old_context);

    ActivePortal = save_active_portal;
    t_thrd.utils_cxt.CurrentResourceOwner = save_resource_owner;
    t_thrd.mem_cxt.portal_mem_cxt = save_portal_context;

    portal->status = PORTAL_READY;
    gstrace_exit(GS_TRC_ID_PortalStart);
}

/*
 * PortalSetResultFormat
 *		Select the format codes for a portal's output.
 *
 * This must be run after PortalStart for a portal that will be read by
 * a DestRemote or DestRemoteExecute destination.  It is not presently needed
 * for other destination types.
 *
 * formats[] is the client format request, as per Bind message conventions.
 */
void PortalSetResultFormat(Portal portal, int nfmts, int16* formats)
{
    int natts;
    int i;

    /* Do nothing if portal won't return tuples */
    if (portal->tupDesc == NULL)
        return;
    natts = portal->tupDesc->natts;
    portal->formats = (int16*)MemoryContextAlloc(PortalGetHeapMemory(portal), natts * sizeof(int16));
    if (nfmts > 1) {
        /* format specified for each column */
        if (nfmts != natts)
            ereport(ERROR,
                (errcode(ERRCODE_PROTOCOL_VIOLATION),
                    errmsg("bind message has %d result formats but query has %d columns", nfmts, natts)));
        errno_t errorno = EOK;
        errorno = memcpy_s(portal->formats, natts * sizeof(int16), formats, natts * sizeof(int16));
        securec_check(errorno, "\0", "\0");
    } else if (nfmts > 0) {
        /* single format specified, use for all columns */
        int16 format1 = formats[0];

        for (i = 0; i < natts; i++)
            portal->formats[i] = format1;
    } else {
        /* use default format for all columns */
        for (i = 0; i < natts; i++)
            portal->formats[i] = 0;
    }
}

/*
 * PortalRun
 *		Run a portal's query or queries.
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".  Note that count is ignored in multi-query
 * situations, where we always run the portal to completion.
 *
 * is_top_level: true if query is being executed at backend "top level"
 * (that is, directly from a client command message)
 *
 * dest: where to send output of primary (canSetTag) query
 *
 * altdest: where to send output of non-primary queries
 *
 * completion_tag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *		May be NULL if caller doesn't want a status string.
 *
 * Returns TRUE if the portal's execution is complete, FALSE if it was
 * suspended due to exhaustion of the count parameter.
 */
bool PortalRun(
    Portal portal, long count, bool is_top_level, DestReceiver* dest, DestReceiver* altdest, char* completion_tag)
{
    gstrace_entry(GS_TRC_ID_PortalRun);
    bool result = false;
    uint64 nprocessed;
    ResourceOwner save_ttx_resource_owner;
    MemoryContext save_ttx_context;
    Portal save_active_portal;
    ResourceOwner save_resource_owner;
    MemoryContext save_portal_context;
    MemoryContext save_memory_context;
    errno_t errorno = EOK;

    AssertArg(PortalIsValid(portal));
    AssertArg(PointerIsValid(portal->commandTag));

    /* match portal->commandTag with CmdType */
    CmdType cmd_type = CMD_UNKNOWN;
    CmdType query_type = CMD_UNKNOWN;
    if (is_top_level && u_sess->attr.attr_common.pgstat_track_activities &&
        u_sess->attr.attr_common.pgstat_track_sql_count && !u_sess->attr.attr_sql.enable_cluster_resize) {
        /*
         * Match at the beginning of PortalRun for
         * portal->commandTag can be changed during process.
         * Only handle the top portal.
         */
        cmd_type = set_cmd_type(portal->commandTag);
        query_type = set_command_type_by_commandTag(portal->commandTag);
    }

    PGSTAT_INIT_TIME_RECORD();

    TRACE_POSTGRESQL_QUERY_EXECUTE_START();

    /* Initialize completion tag to empty string */
    if (completion_tag != NULL)
        completion_tag[0] = '\0';

    if (portal->strategy != PORTAL_MULTI_QUERY) {
        if (u_sess->attr.attr_common.log_executor_stats) {
            elog(DEBUG3, "PortalRun");
            /* PORTAL_MULTI_QUERY logs its own stats per query */
            ResetUsage();
        }
        PGSTAT_START_TIME_RECORD();
    }

    /*
     * Check for improper portal use, and mark portal active.
     */
    MarkPortalActive(portal);

    QueryDesc* query_desc = portal->queryDesc;

    if (IS_PGXC_DATANODE && query_desc != NULL && (query_desc->plannedstmt) != NULL &&
        query_desc->plannedstmt->has_obsrel) {
        increase_rp_number();
    }

    /*
     * Set up global portal context pointers.
     *
     * We have to play a special game here to support utility commands like
     * VACUUM and CLUSTER, which internally start and commit transactions.
     * When we are called to execute such a command, CurrentResourceOwner will
     * be pointing to the TopTransactionResourceOwner --- which will be
     * destroyed and replaced in the course of the internal commit and
     * restart.  So we need to be prepared to restore it as pointing to the
     * exit-time TopTransactionResourceOwner.  (Ain't that ugly?  This idea of
     * internally starting whole new transactions is not good.)
     * CurrentMemoryContext has a similar problem, but the other pointers we
     * save here will be NULL or pointing to longer-lived objects.
     */
    save_ttx_resource_owner = t_thrd.utils_cxt.TopTransactionResourceOwner;
    save_ttx_context = u_sess->top_transaction_mem_cxt;
    save_active_portal = ActivePortal;
    save_resource_owner = t_thrd.utils_cxt.CurrentResourceOwner;
    save_portal_context = t_thrd.mem_cxt.portal_mem_cxt;
    save_memory_context = CurrentMemoryContext;

    if (strcmp(portal->commandTag, "COPY") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "VACUUM") == 0)
        pgstat_set_io_state(IOSTATE_VACUUM);

    if (strcmp(portal->commandTag, "UPDATE") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "INSERT") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "CREATE TABLE") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "ALTER TABLE") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "CREATE INDEX") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "REINDEX INDEX") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "CLUSTER") == 0)
        pgstat_set_io_state(IOSTATE_WRITE);

    if (strcmp(portal->commandTag, "ANALYZE") == 0)
        pgstat_set_io_state(IOSTATE_READ);

    /* set write for backend status for the thread, we will use it to check default transaction readOnly */
    pgstat_set_stmt_tag(STMTTAG_NONE);
    if (strcmp(portal->commandTag, "INSERT") == 0 || strcmp(portal->commandTag, "UPDATE") == 0 ||
        strcmp(portal->commandTag, "CREATE TABLE AS") == 0 || strcmp(portal->commandTag, "CREATE INDEX") == 0 ||
        strcmp(portal->commandTag, "ALTER TABLE") == 0 || strcmp(portal->commandTag, "CLUSTER") == 0)
        pgstat_set_stmt_tag(STMTTAG_WRITE);

    /* workload client manager */
    if (ENABLE_WORKLOAD_CONTROL && query_desc != NULL) {
        WLMTopSQLReady(query_desc);
        WLMInitQueryPlan(query_desc);
        dywlm_client_manager(query_desc);
    }

    PG_TRY();
    {
        ActivePortal = portal;
        t_thrd.utils_cxt.CurrentResourceOwner = portal->resowner;
        t_thrd.mem_cxt.portal_mem_cxt = PortalGetHeapMemory(portal);

        MemoryContextSwitchTo(t_thrd.mem_cxt.portal_mem_cxt);

        switch (portal->strategy) {
            case PORTAL_ONE_SELECT:
            case PORTAL_ONE_RETURNING:
            case PORTAL_ONE_MOD_WITH:
            case PORTAL_UTIL_SELECT:

                /*
                 * If we have not yet run the command, do so, storing its
                 * results in the portal's tuplestore.  But we don't do that
                 * for the PORTAL_ONE_SELECT case.
                 */
                if (portal->strategy != PORTAL_ONE_SELECT && !portal->holdStore) {
                    /* DestRemoteExecute can not send T message automatically */
                    if (strcmp(portal->commandTag, "EXPLAIN") == 0 && dest->mydest != DestRemote)
                        t_thrd.explain_cxt.explain_perf_mode = EXPLAIN_NORMAL;
                    fill_portal_store(portal, is_top_level);
                }

                /*
                 * Now fetch desired portion of results.
                 */
                nprocessed = portal_run_select(portal, true, count, dest);

                /*
                 * If the portal result contains a command tag and the caller
                 * gave us a pointer to store it, copy it. Patch the "SELECT"
                 * tag to also provide the rowcount.
                 */
                if (completion_tag != NULL) {
                    if (strcmp(portal->commandTag, "SELECT") == 0) {
                        errorno = snprintf_s(completion_tag,
                            COMPLETION_TAG_BUFSIZE,
                            COMPLETION_TAG_BUFSIZE - 1,
                            "SELECT %lu",
                            nprocessed);
                        securec_check_ss(errorno, "\0", "\0");
                    } else {
                        errorno = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE,
                            portal->commandTag);
                        securec_check(errorno, "\0", "\0");
                    }
                }

                /* Mark portal not active */
                portal->status = PORTAL_READY;

                /*
                 * Since it's a forward fetch, say DONE iff atEnd is now true.
                 */
                result = portal->atEnd;
                break;

            case PORTAL_MULTI_QUERY:
                portal_run_multi(portal, is_top_level, dest, altdest, completion_tag);

                /* Prevent portal's commands from being re-executed */
                MarkPortalDone(portal);

                /* Always complete at end of RunMulti */
                result = true;
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmodule(MOD_EXECUTOR),
                        errmsg("Unrecognized portal strategy: %d", (int)portal->strategy)));
                result = false; /* keep compiler quiet */
                break;
        }
    }
    PG_CATCH();
    {
        /* Uncaught error while executing portal: mark it dead */
        MarkPortalFailed(portal);

        /* Restore global vars and propagate error */
        if (save_memory_context == save_ttx_context)
            MemoryContextSwitchTo(u_sess->top_transaction_mem_cxt);
        else
            MemoryContextSwitchTo(save_memory_context);
        ActivePortal = save_active_portal;
        if (save_resource_owner == save_ttx_resource_owner)
            t_thrd.utils_cxt.CurrentResourceOwner = t_thrd.utils_cxt.TopTransactionResourceOwner;
        else
            t_thrd.utils_cxt.CurrentResourceOwner = save_resource_owner;
        t_thrd.mem_cxt.portal_mem_cxt = save_portal_context;

        if (ENABLE_WORKLOAD_CONTROL) {
            /* save error to history info */
            save_error_message();
            if (g_instance.wlm_cxt->dynamic_workload_inited) {
                t_thrd.wlm_cxt.parctl_state.errjmp = 1;
                if (t_thrd.wlm_cxt.parctl_state.simple == 0)
                    dywlm_client_release(&t_thrd.wlm_cxt.parctl_state);
                else
                    WLMReleaseGroupActiveStatement();
                dywlm_client_max_release(&t_thrd.wlm_cxt.parctl_state);
            } else
                WLMParctlRelease(&t_thrd.wlm_cxt.parctl_state);

            if (IS_PGXC_COORDINATOR && t_thrd.wlm_cxt.collect_info->sdetail.msg) {
                pfree_ext(t_thrd.wlm_cxt.collect_info->sdetail.msg);
            }
        }

        PG_RE_THROW();
    }
    PG_END_TRY();

    if (ENABLE_WORKLOAD_CONTROL) {
        t_thrd.wlm_cxt.parctl_state.except = 0;

        if (g_instance.wlm_cxt->dynamic_workload_inited && (t_thrd.wlm_cxt.parctl_state.simple == 0)) {
            dywlm_client_release(&t_thrd.wlm_cxt.parctl_state);
        } else {
            // only release resource pool count
            if (IS_PGXC_COORDINATOR && !IsConnFromCoord() &&
                (u_sess->wlm_cxt->parctl_state_exit || IsQueuedSubquery())) {
                WLMReleaseGroupActiveStatement();
            }
        }
        WLMSetCollectInfoStatus(WLM_STATUS_FINISHED);
    }

    if (save_memory_context == save_ttx_context)
        MemoryContextSwitchTo(u_sess->top_transaction_mem_cxt);
    else
        MemoryContextSwitchTo(save_memory_context);
    ActivePortal = save_active_portal;
    if (save_resource_owner == save_ttx_resource_owner)
        t_thrd.utils_cxt.CurrentResourceOwner = t_thrd.utils_cxt.TopTransactionResourceOwner;
    else
        t_thrd.utils_cxt.CurrentResourceOwner = save_resource_owner;
    t_thrd.mem_cxt.portal_mem_cxt = save_portal_context;

    if (portal->strategy != PORTAL_MULTI_QUERY) {
        PGSTAT_END_TIME_RECORD(EXECUTION_TIME);

        if (u_sess->attr.attr_common.log_executor_stats)
            ShowUsage("EXECUTOR STATISTICS");
    }
    TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();

    /* doing sql count accordiong to cmd_type */
    if (cmd_type != CMD_UNKNOWN || query_type != CMD_UNKNOWN) {
        report_qps_type(cmd_type);
        report_qps_type(query_type);
    }

    /* update unique sql stat */
    if (is_top_level && is_unique_sql_enabled() && is_local_unique_sql()) {
        /* Instrumentation: update unique sql returned rows(SELECT) */
        // only CN can update this counter
        if (portal->queryDesc != NULL && portal->queryDesc->estate && portal->queryDesc->estate->es_plannedstmt &&
            portal->queryDesc->estate->es_plannedstmt->commandType == CMD_SELECT) {
            ereport(DEBUG1,
                (errmodule(MOD_INSTR),
                    errmsg("[UniqueSQL]"
                           "unique id: %lu , select returned rows: %lu",
                        u_sess->unique_sql_cxt.unique_sql_id,
                        portal->queryDesc->estate->es_processed)));
            UniqueSQLStatCountReturnedRows(portal->queryDesc->estate->es_processed);
        }

        /* PortalRun using unique_sql_start_time as unique sql elapse start time */
        if (IsNeedUpdateUniqueSQLStat(portal) && IS_UNIQUE_SQL_TRACK_TOP && IsTopUniqueSQL())
            UpdateUniqueSQLStat(NULL, NULL, u_sess->unique_sql_cxt.unique_sql_start_time);

        if (u_sess->unique_sql_cxt.unique_sql_start_time != 0) {
            int64 duration = GetCurrentTimestamp() - u_sess->unique_sql_cxt.unique_sql_start_time;
            if (IS_SINGLE_NODE) {
                pgstat_update_responstime_singlenode(
                    u_sess->unique_sql_cxt.unique_sql_id, u_sess->unique_sql_cxt.unique_sql_start_time, duration);
            } else {
                pgstat_report_sql_rt(
                    u_sess->unique_sql_cxt.unique_sql_id, u_sess->unique_sql_cxt.unique_sql_start_time, duration);
            }
        }
    }
    gstrace_exit(GS_TRC_ID_PortalRun);
    return result;
}

/*
 * portal_run_select
 *		Execute a portal's query in PORTAL_ONE_SELECT mode, and also
 *		when fetching from a completed holdStore in PORTAL_ONE_RETURNING,
 *		PORTAL_ONE_MOD_WITH, and PORTAL_UTIL_SELECT cases.
 *
 * This handles simple N-rows-forward-or-backward cases.  For more complex
 * nonsequential access to a portal, see PortalRunFetch.
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".
 *
 * Caller must already have validated the Portal and done appropriate
 * setup (cf. PortalRun).
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
static uint64 portal_run_select(Portal portal, bool forward, long count, DestReceiver* dest)
{
    QueryDesc* query_desc = NULL;
    ScanDirection direction;
    uint64 nprocessed;

    /*
     * NB: query_desc will be NULL if we are fetching from a held cursor or a
     * completed utility query; can't use it in that path.
     */
    query_desc = PortalGetQueryDesc(portal);

    /* Caller messed up if we have neither a ready query nor held data. */
    AssertEreport(query_desc || portal->holdStore, MOD_EXECUTOR, "have no ready query or held data");

    /*
     * Force the query_desc destination to the right thing.	This supports
     * MOVE, for example, which will pass in dest = DestNone.  This is okay to
     * change as long as we do it on every fetch.  (The Executor must not
     * assume that dest never changes.)
     */
    if (query_desc != NULL)
        query_desc->dest = dest;

    /*
     * Determine which direction to go in, and check to see if we're already
     * at the end of the available tuples in that direction.  If so, set the
     * direction to NoMovement to avoid trying to fetch any tuples.  (This
     * check exists because not all plan node types are robust about being
     * called again if they've already returned NULL once.)  Then call the
     * executor (we must not skip this, because the destination needs to see a
     * setup and shutdown even if no tuples are available).  Finally, update
     * the portal position state depending on the number of tuples that were
     * retrieved.
     */
    if (forward) {
        if (portal->atEnd || count <= 0)
            direction = NoMovementScanDirection;
        else
            direction = ForwardScanDirection;

        /* In the executor, zero count processes all rows */
        if (count == FETCH_ALL)
            count = 0;

        if (portal->holdStore) {
            /* If it`s a explain plan stmt, then we have changed the tag in ExplainQuery. */
            if (strcmp(portal->commandTag, "EXPLAIN") == 0 || strcmp(portal->commandTag, "EXPLAIN SUCCESS") == 0)
                nprocessed = run_from_explain_store(portal, direction, dest);
            else
                nprocessed = run_from_store(portal, direction, count, dest);
        } else {
            if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                PushActiveSnapshot(query_desc->snapshot);
            }

#ifdef PGXC
            if (portal->name != NULL && portal->name[0] != '\0' && IsA(query_desc->planstate, RemoteQueryState)) {
                /*
                 * The snapshot in the query descriptor contains the
                 * command id of the command creating the cursor. We copy
                 * that snapshot in RemoteQueryState so that the do_query
                 * function knows while sending the select (resulting from
                 * a fetch) to the corresponding remote node with the command
                 * id of the command that created the cursor.
                 */
                RemoteQueryState* rqs = (RemoteQueryState*)query_desc->planstate;

                // get the cached scan descriptor in portal
                rqs->ss.ss_currentScanDesc = (AbsTblScanDesc)portal->scanDesc;
                // copy snapshot into the scan descriptor
                portal->scanDesc->rs_snapshot = query_desc->snapshot;
                rqs->cursor = (char*)portal->name;
            }
#endif

            ExecutorRun(query_desc, direction, count);

            /*
             * <<IS_PGXC_COORDINATOR && !StreamTopConsumerAmI()>> means that
             * we are on DWS CN.
             */
            if (IS_PGXC_COORDINATOR && !StreamTopConsumerAmI() && query_desc->plannedstmt->has_obsrel &&
                u_sess->instr_cxt.obs_instr) {
                u_sess->instr_cxt.obs_instr->insertData(query_desc->plannedstmt->queryId);
            }

            nprocessed = query_desc->estate->es_processed;
            if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                PopActiveSnapshot();
            }
        }

        if (!ScanDirectionIsNoMovement(direction)) {
            long old_pos;

            if (nprocessed > 0)
                portal->atStart = false; /* OK to go backward now */
            if (count == 0 || (unsigned long)nprocessed < (unsigned long)count)
                portal->atEnd = true; /* we retrieved 'em all */
            old_pos = portal->portalPos;
            portal->portalPos += nprocessed;
            /* portalPos doesn't advance when we fall off the end */
            if (portal->portalPos < old_pos)
                portal->posOverflow = true;
        }
    } else {
        if ((unsigned int)portal->cursorOptions & CURSOR_OPT_NO_SCROLL)
            ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("Cursor can only scan forward")));

        if (portal->atStart || count <= 0)
            direction = NoMovementScanDirection;
        else
            direction = BackwardScanDirection;

        /* In the executor, zero count processes all rows */
        if (count == FETCH_ALL)
            count = 0;

        if (portal->holdStore)
            nprocessed = run_from_store(portal, direction, count, dest);
        else {
            if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                PushActiveSnapshot(query_desc->snapshot);
            }
            ExecutorRun(query_desc, direction, count);
            nprocessed = query_desc->estate->es_processed;
            if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                PopActiveSnapshot();
            }
        }

        if (!ScanDirectionIsNoMovement(direction)) {
            if (nprocessed > 0 && portal->atEnd) {
                portal->atEnd = false; /* OK to go forward now */
                portal->portalPos++;   /* adjust for endpoint case */
            }
            if (count == 0 || (unsigned long)nprocessed < (unsigned long)count) {
                portal->atStart = true; /* we retrieved 'em all */
                portal->portalPos = 0;
                portal->posOverflow = false;
            } else {
                long old_pos;

                old_pos = portal->portalPos;
                portal->portalPos -= nprocessed;
                if (portal->portalPos > old_pos || portal->portalPos <= 0)
                    portal->posOverflow = true;
            }
        }
    }

    return nprocessed;
}

/*
 * FillPortalStore
 *		Run the query and load result tuples into the portal's tuple store.
 *
 * This is used for PORTAL_ONE_RETURNING, PORTAL_ONE_MOD_WITH, and
 * PORTAL_UTIL_SELECT cases only.
 */
static void fill_portal_store(Portal portal, bool is_top_level)
{
    DestReceiver* treceiver = NULL;
    char completion_tag[COMPLETION_TAG_BUFSIZE];

    PortalCreateHoldStore(portal);
    treceiver = CreateDestReceiver(DestTuplestore);
    SetTuplestoreDestReceiverParams(treceiver, portal->holdStore, portal->holdContext, false);

    completion_tag[0] = '\0';

    switch (portal->strategy) {
        case PORTAL_ONE_RETURNING:
        case PORTAL_ONE_MOD_WITH:

            /*
             * Run the portal to completion just as for the default
             * MULTI_QUERY case, but send the primary query's output to the
             * tuplestore. Auxiliary query outputs are discarded.
             */
            portal_run_multi(portal, is_top_level, treceiver, None_Receiver, completion_tag);
            break;

        case PORTAL_UTIL_SELECT:
            portal_run_utility(portal, (Node*)linitial(portal->stmts), is_top_level, treceiver, completion_tag);
            break;

        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmodule(MOD_EXECUTOR),
                    errmsg("unsupported portal strategy: %d", (int)portal->strategy)));
            break;
    }

    /* Override default completion tag with actual command result */
    if (completion_tag[0] != '\0')
        portal->commandTag = pstrdup(completion_tag);

    (*treceiver->rDestroy)(treceiver);
}

/*
 * RunFromStore
 *		Fetch tuples from the portal's tuple store.
 *
 * Calling conventions are similar to ExecutorRun, except that we
 * do not depend on having a query_desc or estate.  Therefore we return the
 * number of tuples processed as the result, not in estate->es_processed.
 *
 * One difference from ExecutorRun is that the destination receiver functions
 * are run in the caller's memory context (since we have no estate).  Watch
 * out for memory leaks.
 */
static uint32 run_from_store(Portal portal, ScanDirection direction, long count, DestReceiver* dest)
{
    long current_tuple_count = 0;
    TupleTableSlot* slot = NULL;

    slot = MakeSingleTupleTableSlot(portal->tupDesc);

    (*dest->rStartup)(dest, CMD_SELECT, portal->tupDesc);

    if (ScanDirectionIsNoMovement(direction)) {
        /* do nothing except start/stop the destination */
    } else {
        bool forward = ScanDirectionIsForward(direction);

        for (;;) {
            MemoryContext old_context;
            bool ok = false;

            old_context = MemoryContextSwitchTo(portal->holdContext);

            ok = tuplestore_gettupleslot(portal->holdStore, forward, false, slot);

            MemoryContextSwitchTo(old_context);

            if (!ok) {
                break;
            }

            (*dest->receiveSlot)(slot, dest);

            (void)ExecClearTuple(slot);

            /*
             * check our tuple count.. if we've processed the proper number
             * then quit, else loop again and process more tuples. Zero count
             * means no limit.
             */
            current_tuple_count++;
            if (count && count == current_tuple_count) {
                break;
            }
        }
    }

    (*dest->rShutdown)(dest);

    ExecDropSingleTupleTableSlot(slot);

    return (uint32)current_tuple_count;
}

extern uint32 RunGetSlotFromExplain(Portal portal, TupleTableSlot* slot, DestReceiver* dest, int count)
{
    bool forward = true;
    long current_tuple_count = 0;

    (*dest->rStartup)(dest, CMD_SELECT, portal->tupDesc);

    for (;;) {
        MemoryContext old_context;
        bool ok = false;

        old_context = MemoryContextSwitchTo(portal->holdContext);

        ok = tuplestore_gettupleslot(portal->holdStore, forward, false, slot);

        MemoryContextSwitchTo(old_context);

        if (!ok) {
            break;
        }

        (*dest->receiveSlot)(slot, dest);

        (void)ExecClearTuple(slot);

        current_tuple_count++;
        if (count && count == current_tuple_count) {
            break;
        }
    }

    (*dest->rShutdown)(dest);

    return (uint32)current_tuple_count;
}

static uint32 run_from_explain_store(Portal portal, ScanDirection direction, DestReceiver* dest)
{
    long current_tuple_count = 0;
    TupleTableSlot* slot = NULL;
    ExplainStmt* stmt = NULL;
    PlanInformation* planinfo = NULL;

    if (IsA((Node*)linitial(portal->stmts), ExplainStmt))
        stmt = (ExplainStmt*)((Node*)linitial(portal->stmts));
    if (stmt == NULL)
        return 0;

    planinfo = stmt->planinfo;

    if (t_thrd.explain_cxt.explain_perf_mode != EXPLAIN_NORMAL && planinfo) {
        portal->formats = NULL;
        current_tuple_count = planinfo->print_plan(portal, dest);
    } else {
        slot = MakeSingleTupleTableSlot(portal->tupDesc);
        current_tuple_count += RunGetSlotFromExplain(portal, slot, dest, 0);
        ExecDropSingleTupleTableSlot(slot);
    }

    (*dest->rShutdown)(dest);

    return (uint32)current_tuple_count;
}

/*
 * portal_run_utility
 *		Execute a utility statement inside a portal.
 */
static void portal_run_utility(Portal portal, Node* utility_stmt, bool is_top_level, DestReceiver* dest, char* completion_tag)
{
    bool active_snapshot_set = false;

    elog(DEBUG3, "ProcessUtility");

    /*
     * Set snapshot if utility stmt needs one.	Most reliable way to do this
     * seems to be to enumerate those that do not need one; this is a short
     * list.  Transaction control, LOCK, and SET must *not* set a snapshot
     * since they need to be executable at the start of a transaction-snapshot
     * mode transaction without freezing a snapshot.  By extension we allow
     * SHOW not to set a snapshot.	The other stmts listed are just efficiency
     * hacks.  Beware of listing anything that can modify the database --- if,
     * say, it has to update an index with expressions that invoke
     * user-defined functions, then it had better have a snapshot.
     */
    if (!(portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM) &&
            !(IsA(utility_stmt, TransactionStmt) || IsA(utility_stmt, LockStmt) || IsA(utility_stmt, VariableSetStmt) ||
            IsA(utility_stmt, VariableShowStmt) || IsA(utility_stmt, ConstraintsSetStmt) ||
            /* efficiency hacks from here down */
            IsA(utility_stmt, FetchStmt) || IsA(utility_stmt, ListenStmt) || IsA(utility_stmt, NotifyStmt) ||
            IsA(utility_stmt, UnlistenStmt) ||
#ifdef PGXC
            (IsA(utility_stmt, CheckPointStmt) && IS_PGXC_DATANODE)))
#else
            IsA(utility_stmt, CheckPointStmt)))
#endif
    {
        PushActiveSnapshot(GetTransactionSnapshot());
        active_snapshot_set = true;
    } else
        active_snapshot_set = false;

    /* Exec workload client manager if commandTag is not EXPLAIN or EXECUTE */
    if (ENABLE_WORKLOAD_CONTROL)
        WLMSetExecutorStartTime();

    ProcessUtility(utility_stmt,
        portal->sourceText,
        portal->portalParams,
        is_top_level,
        dest,
#ifdef PGXC
        false,
#endif /* PGXC */
        completion_tag);

    /* Some utility statements may change context on us */
    MemoryContextSwitchTo(PortalGetHeapMemory(portal));

    /*
     * Some utility commands may pop the u_sess->utils_cxt.ActiveSnapshot stack from under us,
     * so we only pop the stack if we actually see a snapshot set.	Note that
     * the set of utility commands that do this must be the same set
     * disallowed to run inside a transaction; otherwise, we could be popping
     * a snapshot that belongs to some other operation.
     */
    if (active_snapshot_set && ActiveSnapshotSet())
        PopActiveSnapshot();

    perm_space_value_reset();
}

/*
 * portal_run_multi
 *		Execute a portal's queries in the general case (multi queries
 *		or non-SELECT-like queries)
 */
static void portal_run_multi(
    Portal portal, bool is_top_level, DestReceiver* dest, DestReceiver* altdest, char* completion_tag)
{
    bool active_snapshot_set = false;
    ListCell* stmtlist_item = NULL;
    PGSTAT_INIT_TIME_RECORD();
#ifdef PGXC
    CombineTag combine;

    combine.cmdType = CMD_UNKNOWN;
    combine.data[0] = '\0';
#endif

    bool force_local_snapshot = false;
    
    if ((portal != NULL) && (portal->cplan != NULL)) {
        /* copy over the single_shard_stmt into local variable force_local_snapshot */
        force_local_snapshot = portal->cplan->single_shard_stmt;
    }
    /*
     * If the destination is DestRemoteExecute, change to DestNone.  The
     * reason is that the client won't be expecting any tuples, and indeed has
     * no way to know what they are, since there is no provision for Describe
     * to send a RowDescription message when this portal execution strategy is
     * in effect.  This presently will only affect SELECT commands added to
     * non-SELECT queries by rewrite rules: such commands will be executed,
     * but the results will be discarded unless you use "simple Query"
     * protocol.
     */
    if (dest->mydest == DestRemoteExecute)
        dest = None_Receiver;
    if (altdest->mydest == DestRemoteExecute)
        altdest = None_Receiver;

    /* sql active feature: create table as case */
    uint32 instrument_option = 0;
    if (IS_PGXC_COORDINATOR && u_sess->attr.attr_resource.resource_track_level == RESOURCE_TRACK_OPERATOR &&
        IS_STREAM && u_sess->attr.attr_resource.use_workload_manager &&
        t_thrd.wlm_cxt.collect_info->status != WLM_STATUS_RUNNING && IsSupportExplain(portal->commandTag) &&
        !u_sess->attr.attr_sql.enable_cluster_resize) {
        instrument_option |= INSTRUMENT_TIMER;
        instrument_option |= INSTRUMENT_BUFFERS;
    }

    /*
     * Loop to handle the individual queries generated from a single parsetree
     * by analysis and rewrite.
     */
    foreach (stmtlist_item, portal->stmts) {
        Node* stmt = (Node*)lfirst(stmtlist_item);
        bool isMMTable = false;
        JitExec::JitContext* mot_jit_context = NULL;

        /*
         * If we got a cancel signal in prior command, quit
         */
        CHECK_FOR_INTERRUPTS();

        if (IsA(stmt, PlannedStmt) && ((PlannedStmt*)stmt)->utilityStmt == NULL) {
            /*
             * process a plannable query.
             */
            PlannedStmt* pstmt = (PlannedStmt*)stmt;

            TRACE_POSTGRESQL_QUERY_EXECUTE_START();

            if (u_sess->attr.attr_common.log_executor_stats)
                ResetUsage();

            PGSTAT_START_TIME_RECORD();

            /*
             * Must always have a snapshot for plannable queries, unless it is a MM query.
             * First time through, take a new snapshot; for subsequent queries in the
             * same portal, just update the snapshot's copy of the command
             * counter.
             */
            if ((portal->cplan != NULL && portal->cplan->storageEngineType == SE_TYPE_MM)) {
                isMMTable = true;
                mot_jit_context = portal->cplan->mot_jit_context;
            }

            if (!isMMTable) {
                if (!active_snapshot_set) {
                    PushActiveSnapshot(GetTransactionSnapshot(force_local_snapshot));
                    active_snapshot_set = true;
                } else
                    UpdateActiveSnapshotCommandId();
            }

            if (IS_PGXC_COORDINATOR || IS_SINGLE_NODE)
                pstmt->instrument_option = instrument_option;

            if (pstmt->canSetTag) {
                /* statement can set tag string */
                process_query(pstmt, portal->sourceText, portal->portalParams, 
                    isMMTable, mot_jit_context, dest, completion_tag);
#ifdef PGXC
                /* it's special for INSERT */
                if (IS_PGXC_COORDINATOR && pstmt->commandType == CMD_INSERT)
                    HandleCmdComplete(pstmt->commandType, &combine, completion_tag, strlen(completion_tag));
#endif
            } else {
                /* stmt added by rewrite cannot set tag */
                process_query(pstmt, portal->sourceText, portal->portalParams, 
                    isMMTable, mot_jit_context, altdest, NULL);
            }

            PGSTAT_END_TIME_RECORD(EXECUTION_TIME);

            if (u_sess->attr.attr_common.log_executor_stats)
                ShowUsage("EXECUTOR STATISTICS");

            TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();
        } else {
            /*
             * process utility functions (create, destroy, etc..)
             *
             * These are assumed canSetTag if they're the only stmt in the
             * portal.
             *
             * We must not set a snapshot here for utility commands (if one is
             * needed, portal_run_utility will do it).  If a utility command is
             * alone in a portal then everything's fine.  The only case where
             * a utility command can be part of a longer list is that rules
             * are allowed to include NotifyStmt.  NotifyStmt doesn't care
             * whether it has a snapshot or not, so we just leave the current
             * snapshot alone if we have one.
             */
            if (list_length(portal->stmts) == 1) {
                AssertEreport(!active_snapshot_set, MOD_EXECUTOR, "No active snapshot for utility commands");
                /* statement can set tag string */
                portal_run_utility(portal, stmt, is_top_level, dest, completion_tag);
            } else {
                AssertEreport(IsA(stmt, NotifyStmt), MOD_EXECUTOR, "Not a NotifyStmt");
                /* stmt added by rewrite cannot set tag */
                portal_run_utility(portal, stmt, is_top_level, altdest, NULL);
            }
        }

        /*
         * Increment command counter between queries, but not after the last
         * one.
         */
        if (lnext(stmtlist_item) != NULL)
            CommandCounterIncrement();

        /*
         * Clear subsidiary contexts to recover temporary memory.
         */
        AssertEreport(
            PortalGetHeapMemory(portal) == CurrentMemoryContext, MOD_EXECUTOR, "Memory context is not consistant");

        MemoryContextDeleteChildren(PortalGetHeapMemory(portal));
    }

    /* Pop the snapshot if we pushed one. */
    if (active_snapshot_set)
        PopActiveSnapshot();

    /*
     * If a command completion tag was supplied, use it.  Otherwise use the
     * portal's commandTag as the default completion tag.
     *
     * Exception: Clients expect INSERT/UPDATE/DELETE tags to have counts, so
     * fake them with zeros.  This can happen with DO INSTEAD rules if there
     * is no replacement query of the same type as the original.  We print "0
     * 0" here because technically there is no query of the matching tag type,
     * and printing a non-zero count for a different query type seems wrong,
     * e.g.  an INSERT that does an UPDATE instead should not print "0 1" if
     * one row was updated.  See QueryRewrite(), step 3, for details.
     */
    errno_t errorno = EOK;
#ifdef PGXC
    if (IS_PGXC_COORDINATOR && completion_tag != NULL && combine.data[0] != '\0') {
        errorno = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE, combine.data);
        securec_check(errorno, "\0", "\0");
    }
#endif

    if (completion_tag != NULL && completion_tag[0] == '\0') {
        if (portal->commandTag) {
            errorno = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE, portal->commandTag);
            securec_check(errorno, "\0", "\0");
        }
        if (strcmp(completion_tag, "SELECT") == 0) {
            errorno = sprintf_s(completion_tag, COMPLETION_TAG_BUFSIZE, "SELECT 0 0");
            securec_check_ss(errorno, "\0", "\0");
        } else if (strcmp(completion_tag, "INSERT") == 0) {
            errorno = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE, "INSERT 0 0");
            securec_check(errorno, "\0", "\0");
        } else if (strcmp(completion_tag, "UPDATE") == 0) {
            errorno = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE, "UPDATE 0");
            securec_check(errorno, "\0", "\0");
        } else if (strcmp(completion_tag, "DELETE") == 0) {
            errorno = strcpy_s(completion_tag, COMPLETION_TAG_BUFSIZE, "DELETE 0");
            securec_check(errorno, "\0", "\0");
        }
    }
}

/*
 * PortalRunFetch
 *		Variant form of PortalRun that supports SQL FETCH directions.
 *
 * Note: we presently assume that no callers of this want is_top_level = true.
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
long PortalRunFetch(Portal portal, FetchDirection fdirection, long count, DestReceiver* dest)
{
    long result;
    Portal save_active_portal;
    ResourceOwner save_resource_owner;
    MemoryContext save_portal_context;
    MemoryContext old_context;

    AssertArg(PortalIsValid(portal));

    /*
     * Check for improper portal use, and mark portal active.
     */
    MarkPortalActive(portal);

    /* Disable early free when using cursor which may need rescan */
    bool saved_early_free = u_sess->attr.attr_sql.enable_early_free;
    u_sess->attr.attr_sql.enable_early_free = false;

    /*
     * Set up global portal context pointers.
     */
    save_active_portal = ActivePortal;
    save_resource_owner = t_thrd.utils_cxt.CurrentResourceOwner;
    save_portal_context = t_thrd.mem_cxt.portal_mem_cxt;
    PG_TRY();
    {
        ActivePortal = portal;
        t_thrd.utils_cxt.CurrentResourceOwner = portal->resowner;
        t_thrd.mem_cxt.portal_mem_cxt = PortalGetHeapMemory(portal);

        old_context = MemoryContextSwitchTo(t_thrd.mem_cxt.portal_mem_cxt);

        switch (portal->strategy) {
            case PORTAL_ONE_SELECT:
                result = do_portal_run_fetch(portal, fdirection, count, dest);
                break;

            case PORTAL_ONE_RETURNING:
            case PORTAL_ONE_MOD_WITH:
            case PORTAL_UTIL_SELECT:

                /*
                 * If we have not yet run the command, do so, storing its
                 * results in the portal's tuplestore.
                 */
                if (!portal->holdStore) {
                    /* DestRemoteExecute can not send T message automatically */
                    if (strcmp(portal->commandTag, "EXPLAIN") == 0 && dest->mydest != DestRemote)
                        t_thrd.explain_cxt.explain_perf_mode = EXPLAIN_NORMAL;
                    fill_portal_store(portal, false /* is_top_level */);
                }

                /*
                 * Now fetch desired portion of results.
                 */
                result = do_portal_run_fetch(portal, fdirection, count, dest);
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmodule(MOD_EXECUTOR),
                        errmsg("unsupported portal strategy")));
                result = 0; /* keep compiler quiet */
                break;
        }
    }
    PG_CATCH();
    {
        /* Uncaught error while executing portal: mark it dead */
        MarkPortalFailed(portal);

        /* Restore global vars and propagate error */
        ActivePortal = save_active_portal;
        t_thrd.utils_cxt.CurrentResourceOwner = save_resource_owner;
        t_thrd.mem_cxt.portal_mem_cxt = save_portal_context;

        /* Restore GUC variable enable_early_free */
        u_sess->attr.attr_sql.enable_early_free = saved_early_free;

        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old_context);

    /* Mark portal not active */
    portal->status = PORTAL_READY;

    ActivePortal = save_active_portal;
    t_thrd.utils_cxt.CurrentResourceOwner = save_resource_owner;
    t_thrd.mem_cxt.portal_mem_cxt = save_portal_context;

    /* Restore GUC variable enable_early_free */
    u_sess->attr.attr_sql.enable_early_free = saved_early_free;

    return result;
}

/*
 * do_portal_run_fetch
 *		Guts of PortalRunFetch --- the portal context is already set up
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
static long do_portal_run_fetch(Portal portal, FetchDirection fdirection, long count, DestReceiver* dest)
{
    bool forward = false;

    AssertEreport(portal->strategy == PORTAL_ONE_SELECT || portal->strategy == PORTAL_ONE_RETURNING ||
                      portal->strategy == PORTAL_ONE_MOD_WITH || portal->strategy == PORTAL_UTIL_SELECT,
        MOD_EXECUTOR,
        "portal strategy is not select, returning, mod_with, or util select");

    /* workload client manager */
    if (ENABLE_WORKLOAD_CONTROL && portal->queryDesc && !portal->queryDesc->executed) {
        if (IS_PGXC_COORDINATOR || IS_SINGLE_NODE) {
            /* Check if need track resource */
            u_sess->exec_cxt.need_track_resource = WLMNeedTrackResource(portal->queryDesc);

            /* Add the definition of CURSOR to the end of the query */
            if (u_sess->exec_cxt.need_track_resource && t_thrd.wlm_cxt.collect_info->sdetail.statement &&
                portal->queryDesc->sourceText && !t_thrd.wlm_cxt.has_cursor_record) {
                USE_MEMORY_CONTEXT(g_instance.wlm_cxt->query_resource_track_mcxt);

                pgstat_set_io_state(IOSTATE_READ);

                uint32 query_str_len = strlen(t_thrd.wlm_cxt.collect_info->sdetail.statement) +
                                       strlen(portal->queryDesc->sourceText) + 3; /* 3 is the length of "()" and '\0' */
                char* query_str = (char*)palloc0(query_str_len);
                int rc = snprintf_s(query_str,
                    query_str_len,
                    query_str_len - 1,
                    "%s(%s)",
                    t_thrd.wlm_cxt.collect_info->sdetail.statement,
                    portal->queryDesc->sourceText);
                securec_check_ss(rc, "\0", "\0");

                pfree_ext(t_thrd.wlm_cxt.collect_info->sdetail.statement);
                t_thrd.wlm_cxt.collect_info->sdetail.statement = query_str;

                uint32 hashcode = WLMHashCode(&u_sess->wlm_cxt->wlm_params.qid, sizeof(Qid));
                LockSessRealTHashPartition(hashcode, LW_EXCLUSIVE);
                WLMDNodeInfo* info = (WLMDNodeInfo*)hash_search(g_instance.wlm_cxt->stat_manager.collect_info_hashtbl,
                    &u_sess->wlm_cxt->wlm_params.qid,
                    HASH_FIND,
                    NULL);
                if (info != NULL) {
                    pfree_ext(info->statement);
                    info->statement = pstrdup(t_thrd.wlm_cxt.collect_info->sdetail.statement);
                    t_thrd.wlm_cxt.has_cursor_record = true;
                }

                UnLockSessRealTHashPartition(hashcode);
            }
        }

        WLMInitQueryPlan(portal->queryDesc);
        dywlm_client_manager(portal->queryDesc);
    }

    switch (fdirection) {
        case FETCH_FORWARD:
            if (count < 0) {
                fdirection = FETCH_BACKWARD;
                count = -count;
            }
            /* fall out of switch to share code with FETCH_BACKWARD */
            break;
        case FETCH_BACKWARD:
            if (count < 0) {
                fdirection = FETCH_FORWARD;
                count = -count;
            }
            /* fall out of switch to share code with FETCH_FORWARD */
            break;
        case FETCH_ABSOLUTE:
            if (count > 0) {
                /*
                 * Definition: Rewind to start, advance count-1 rows, return
                 * next row (if any).  If the goal is less than portalPos,
                 * we need to rewind, or we can fetch the target row forwards.
                 */
                if (portal->posOverflow || portal->portalPos == LONG_MAX || count - 1 < portal->portalPos) {
                    do_portal_rewind(portal);
                    if (count > 1)
                        (void)portal_run_select(portal, true, count - 1, None_Receiver);
                } else {
                    long pos = portal->portalPos;

                    if (portal->atEnd)
                        pos++; /* need one extra fetch if off end */
                    if (count <= pos)
                        (void)portal_run_select(portal, false, pos - count + 1, None_Receiver);
                    else if (count > pos + 1)
                        (void)portal_run_select(portal, true, count - pos - 1, None_Receiver);
                }
                return portal_run_select(portal, true, 1L, dest);
            } else if (count < 0) {
                /*
                 * Definition: Advance to end, back up abs(count)-1 rows,
                 * return prior row (if any).  We could optimize this if we
                 * knew in advance where the end was, but typically we won't.
                 * (Is it worth considering case where count > half of size of
                 * query?  We could rewind once we know the size ...)
                 */
                (void)portal_run_select(portal, true, FETCH_ALL, None_Receiver);
                if (count < -1)
                    (void)portal_run_select(portal, false, -count - 1, None_Receiver);
                return portal_run_select(portal, false, 1L, dest);
            } else {
                /* Rewind to start, return zero rows */
                do_portal_rewind(portal);
                return portal_run_select(portal, true, 0L, dest);
            }
            break;
        case FETCH_RELATIVE:
            if (count > 0) {
                /*
                 * Definition: advance count-1 rows, return next row (if any).
                 */
                if (count > 1)
                    (void)portal_run_select(portal, true, count - 1, None_Receiver);
                return portal_run_select(portal, true, 1L, dest);
            } else if (count < 0) {
                /*
                 * Definition: back up abs(count)-1 rows, return prior row (if
                 * any).
                 */
                if (count < -1)
                    (void)portal_run_select(portal, false, -count - 1, None_Receiver);
                return portal_run_select(portal, false, 1L, dest);
            } else {
                /* Same as FETCH FORWARD 0, so fall out of switch */
                fdirection = FETCH_FORWARD;
            }
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmodule(MOD_EXECUTOR),
                    errmsg("bogus direction")));
            break;
    }

    /*
     * Get here with fdirection == FETCH_FORWARD or FETCH_BACKWARD, and count
     * >= 0.
     */
    forward = (fdirection == FETCH_FORWARD);

    /*
     * Zero count means to re-fetch the current row, if any (per SQL92)
     */
    if (count == 0) {
        bool on_row = false;

        /* Are we sitting on a row? */
        on_row = (!portal->atStart && !portal->atEnd);

        if (dest->mydest == DestNone) {
            /* MOVE 0 returns 0/1 based on if FETCH 0 would return a row */
            return on_row ? 1L : 0L;
        } else {
            /*
             * If we are sitting on a row, back up one so we can re-fetch it.
             * If we are not sitting on a row, we still have to start up and
             * shut down the executor so that the destination is initialized
             * and shut down correctly; so keep going.	To portal_run_select,
             * count == 0 means we will retrieve no row.
             */
            if (on_row) {
                (void)portal_run_select(portal, false, 1L, None_Receiver);
                /* Set up to fetch one row forward */
                count = 1;
                forward = true;
            }
        }
    }

    /*
     * Optimize MOVE BACKWARD ALL into a Rewind.
     */
    if (!forward && count == FETCH_ALL && dest->mydest == DestNone) {
        long result = portal->portalPos;

        if (result > 0 && !portal->atEnd)
            result--;
        do_portal_rewind(portal);
        /* result is bogus if pos had overflowed, but it's best we can do */
        return result;
    }

    return portal_run_select(portal, forward, count, dest);
}

/*
 * do_portal_rewind - rewind a Portal to starting point
 */
static void do_portal_rewind(Portal portal)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmodule(MOD_EXECUTOR), errmsg("Cursor rewind are not supported.")));

    if (portal->holdStore) {
        MemoryContext old_context;

        old_context = MemoryContextSwitchTo(portal->holdContext);
        tuplestore_rescan(portal->holdStore);
        MemoryContextSwitchTo(old_context);
    }
    if (PortalGetQueryDesc(portal))
        ExecutorRewind(PortalGetQueryDesc(portal));

    portal->atStart = true;
    portal->atEnd = false;
    portal->portalPos = 0;
    portal->posOverflow = false;
}
