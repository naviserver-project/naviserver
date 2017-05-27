/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * filter.c --
 *
 * Support for connection filters, traces, and cleanups.
 */

#include "nsd.h"

/*
 * The following stuctures maintain connection filters
 * and traces.       
 */

typedef struct Filter {
    struct Filter *nextPtr;
    Ns_FilterProc *proc;
    const char    *method;
    const char    *url;
    Ns_FilterType  when;
    void          *arg;
} Filter;

typedef struct Trace {
    struct Trace    *nextPtr;
    Ns_TraceProc    *proc;
    void            *arg;
} Trace;

static Trace *NewTrace(Ns_TraceProc *proc, void *arg)
    NS_GNUC_NONNULL(1);

static void RunTraces(Ns_Conn *conn, const Trace *tracePtr)
    NS_GNUC_NONNULL(1);

static void *RegisterCleanup(NsServer *servPtr, Ns_TraceProc *proc, void *arg)
    NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 * Ns_RegisterFilter --
 *
 *      Register a filter function to handle a method/URL combination.
 *
 * Results:
 *      Returns a pointer to an opaque object that contains the filter
 *	information.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterFilter(const char *server, const char *method, const char *url,
                  Ns_FilterProc *proc, Ns_FilterType when, void *arg, bool first)
{
    NsServer *servPtr;
    Filter *fPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetServer(server);
    assert(servPtr != NULL);

    fPtr = ns_malloc(sizeof(Filter));
    fPtr->proc = proc;
    fPtr->method = ns_strdup(method);
    fPtr->url = ns_strdup(url);
    fPtr->when = when;
    fPtr->arg = arg;
    Ns_MutexLock(&servPtr->filter.lock);
    if (first) {
        fPtr->nextPtr = servPtr->filter.firstFilterPtr;
        servPtr->filter.firstFilterPtr = fPtr;
    } else {
        Filter **fPtrPtr;

        fPtr->nextPtr = NULL;
        fPtrPtr = &servPtr->filter.firstFilterPtr;
        while (*fPtrPtr != NULL) {
            fPtrPtr = &((*fPtrPtr)->nextPtr);
        }
        *fPtrPtr = fPtr;
    }
    Ns_MutexUnlock(&servPtr->filter.lock);
    return (void *) fPtr;
}


/*
 *----------------------------------------------------------------------
 * NsRunFilters --
 *
 *      Execute each registered filter function in the Filter list.
 *
 * Results:
 *      Returns the status returned from the registered filter function.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsRunFilters(Ns_Conn *conn, Ns_FilterType why)
{
    NsServer      *servPtr;
    const Filter  *fPtr;
    Ns_ReturnCode  status;

    NS_NONNULL_ASSERT(conn != NULL);
    servPtr = ((const Conn *)conn)->poolPtr->servPtr;

    status = NS_OK;
    if ((conn->request.method != NULL) && (conn->request.url != NULL)) {
        Ns_ReturnCode filter_status = NS_OK;
        
        Ns_MutexLock(&servPtr->filter.lock);
	fPtr = servPtr->filter.firstFilterPtr;
	while (fPtr != NULL && filter_status == NS_OK) {
	    if (unlikely(fPtr->when == why)
		&& (Tcl_StringMatch(conn->request.method, fPtr->method) != 0)
		&& (Tcl_StringMatch(conn->request.url, fPtr->url) != 0)) {
	        Ns_MutexUnlock(&servPtr->filter.lock);
		filter_status = (*fPtr->proc)(fPtr->arg, conn, why);
		Ns_MutexLock(&servPtr->filter.lock);
	    }
	    fPtr = fPtr->nextPtr;
	}
	Ns_MutexUnlock(&servPtr->filter.lock);
	if (filter_status == NS_FILTER_BREAK ||
	    (why == NS_FILTER_TRACE && filter_status == NS_FILTER_RETURN)) {
	    status = NS_OK;
	} else {
            status = filter_status;
        }
    }

    /*
     * We can get 
     *
     *    status == NS_FILTER_RETURN 
     *
     * for e.g.    why == NS_FILTER_PRE_AUTH 
     * but not for why == NS_FILTER_TRACE
     */

    assert(status == NS_OK || status == NS_ERROR || status == NS_FILTER_RETURN || status == NS_FILTER_BREAK);

    return status;
}


/*
 *----------------------------------------------------------------------
 * Ns_RegisterServerTrace --
 *
 *      Register a connection trace procedure.  Traces registered
 *  	with this procedure are only called in FIFO order if the
 *  	connection request procedure successfully responds to the
 *  	clients request.
 *
 * Results:
 *	Pointer to trace.
 *
 * Side effects:
 *      Proc will be called in FIFO order at end of successful
 *  	connections.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterServerTrace(const char *server, Ns_TraceProc *proc, void *arg)
{
    NsServer *servPtr;
    Trace *tracePtr, **tPtrPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetServer(server);
    assert(servPtr != NULL);

    tracePtr = NewTrace(proc, arg);
    tPtrPtr = &servPtr->filter.firstTracePtr;
    while (*tPtrPtr != NULL) {
    	tPtrPtr = &((*tPtrPtr)->nextPtr);
    }
    *tPtrPtr = tracePtr;
    tracePtr->nextPtr = NULL;

    return (void *) tracePtr;
}


/*
 *----------------------------------------------------------------------
 * Ns_RegisterCleanup, Ns_RegisterConnCleanup --
 *
 *      Register a connection cleanup trace procedure.  Traces
 *  	registered with this procedure are always called in LIFO
 *  	order at the end of connection no matter the result code
 *  	from the connection's request procedure (i.e., the procs
 *  	are called even if the client drops connection).
 *
 * Results:
 *	Pointer to trace.
 *
 * Side effects:
 *      Proc will be called in LIFO order at end of all connections.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterConnCleanup(const char *server, Ns_TraceProc *proc, void *arg)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetServer(server);
    return RegisterCleanup(servPtr, proc, arg);
}

void *
Ns_RegisterCleanup(Ns_TraceProc *proc, void *arg)
{
    NsServer *servPtr = NsGetInitServer();

    NS_NONNULL_ASSERT(proc != NULL);

    return RegisterCleanup(servPtr, proc, arg);
}

static void *
RegisterCleanup(NsServer *servPtr, Ns_TraceProc *proc, void *arg)
{
    Trace *tracePtr = NULL;

    NS_NONNULL_ASSERT(proc != NULL);

    if (servPtr != NULL) {
        tracePtr = NewTrace(proc, arg);
        tracePtr->nextPtr = servPtr->filter.firstCleanupPtr;
        servPtr->filter.firstCleanupPtr = tracePtr;
    }
    return (void *) tracePtr;
}


/*
 *----------------------------------------------------------------------
 * RunTraces, NsRunTraces, NsRunCleanups --
 *
 *      Execute each registered trace.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Depends on registered traces, if any.
 *
 *----------------------------------------------------------------------
 */

void
NsRunTraces(Ns_Conn *conn)
{
    NS_NONNULL_ASSERT(conn != NULL);

    RunTraces(conn, ((const Conn *) conn)->poolPtr->servPtr->filter.firstTracePtr);
}

void
NsRunCleanups(Ns_Conn *conn)
{
    NS_NONNULL_ASSERT(conn != NULL);

    RunTraces(conn, ((const Conn *) conn)->poolPtr->servPtr->filter.firstCleanupPtr);
}

static void
RunTraces(Ns_Conn *conn, const Trace *tracePtr)
{
    NS_NONNULL_ASSERT(conn != NULL);

    while (tracePtr != NULL) {
    	(*tracePtr->proc)(tracePtr->arg, conn);
	tracePtr = tracePtr->nextPtr;
    }
}


/*
 *----------------------------------------------------------------------
 * NewTrace --
 *
 *      Create a new trace object to be added to the cleanup or
 *	trace list.
 *
 * Results:
 *      ns_malloc'ed trace structure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Trace *
NewTrace(Ns_TraceProc *proc, void *arg)
{
    Trace *tracePtr;

    NS_NONNULL_ASSERT(proc != NULL);

    tracePtr = ns_malloc(sizeof(Trace));
    tracePtr->proc = proc;
    tracePtr->arg = arg;
    return tracePtr;
}


/*
 *----------------------------------------------------------------------
 * NsGetTraces, NsGetFilters --
 *
 *      Returns information about registered filters/traces
 *
 * Results:
 *      DString with info as Tcl list
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

void
NsGetFilters(Tcl_DString *dsPtr, const char *server)
{
    const NsServer *servPtr;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);

    servPtr = NsGetServer(server);

    if (servPtr != NULL) {
        const Filter *fPtr;
        
        for (fPtr = servPtr->filter.firstFilterPtr; fPtr != NULL; fPtr = fPtr->nextPtr) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppendElement(dsPtr, fPtr->method);
            Tcl_DStringAppendElement(dsPtr, fPtr->url);
            
            switch (fPtr->when) {
            case NS_FILTER_PRE_AUTH:
                Tcl_DStringAppendElement(dsPtr, "preauth");
                break;
            case NS_FILTER_POST_AUTH:
                Tcl_DStringAppendElement(dsPtr, "postauth");
                break;
            case NS_FILTER_VOID_TRACE: 
            case NS_FILTER_TRACE:
                Tcl_DStringAppendElement(dsPtr, "trace");
                break;
            }
            Ns_GetProcInfo(dsPtr, (Ns_Callback *)fPtr->proc, fPtr->arg);
            Tcl_DStringEndSublist(dsPtr);
        }
    }
}   

void
NsGetTraces(Tcl_DString *dsPtr, const char *server)
{
    const NsServer *servPtr;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);
    
    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        const Trace *tracePtr;

        tracePtr = servPtr->filter.firstTracePtr;
        while (tracePtr != NULL) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppendElement(dsPtr, "trace");
            Ns_GetProcInfo(dsPtr, (Ns_Callback *)tracePtr->proc, tracePtr->arg);
            Tcl_DStringEndSublist(dsPtr);
            tracePtr = tracePtr->nextPtr;
        }

        tracePtr = servPtr->filter.firstCleanupPtr;
        while (tracePtr != NULL) {
            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppendElement(dsPtr, "cleanup");
            Ns_GetProcInfo(dsPtr, (Ns_Callback *)tracePtr->proc, tracePtr->arg);
            Tcl_DStringEndSublist(dsPtr);
            tracePtr = tracePtr->nextPtr;
        }
    }
}   

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
