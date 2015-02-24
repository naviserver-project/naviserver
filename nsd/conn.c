/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
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
 * conn.c --
 *
 *      Manage the Ns_Conn structure
 */

#include "nsd.h"

static int GetChan(Tcl_Interp *interp, const char *id, Tcl_Channel *chanPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int GetIndices(Tcl_Interp *interp, const Conn *connPtr, Tcl_Obj *CONST* objv,
                      int *offPtr, int *lenPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Tcl_Channel MakeConnChannel(const NsInterp *itPtr, Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnHeaders --
 *
 *      Get the headers
 *
 * Results:
 *      An Ns_Set containing HTTP headers from the client
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_ConnHeaders(const Ns_Conn *conn)
{
    assert(conn != NULL);
    return conn->headers;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnOutputHeaders --
 *
 *      Get the output headers
 *
 * Results:
 *      A writeable Ns_Set containing headers to send back to the client
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_ConnOutputHeaders(const Ns_Conn *conn)
{
    assert(conn != NULL);
    return conn->outputheaders;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnAuth --
 *
 *      Get the authentication headers
 *
 * Results:
 *      An Ns_Set containing authentication user/password and other parameters
 *      as in digest method
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_ConnAuth(const Ns_Conn *conn)
{
    assert(conn != NULL);
    return conn->auth;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnAuthUser --
 *
 *      Get the authenticated user
 *
 * Results:
 *      A pointer to a string with the username
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnAuthUser(const Ns_Conn *conn)
{
    assert(conn != NULL);
    return conn->auth != NULL ? Ns_SetIGet(conn->auth, "Username") : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnAuthPasswd --
 *
 *      Get the authenticated user's password
 *
 * Results:
 *      A pointer to a string with the user's plaintext password
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnAuthPasswd(const Ns_Conn *conn)
{
    assert(conn != NULL);
    return conn->auth != NULL ? Ns_SetIGet(conn->auth, "Password") : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentLength --
 *
 *      Get the content length from the client
 *
 * Results:
 *      An size_t content length, or 0 if none sent
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_ConnContentLength(const Ns_Conn *conn)
{
    assert(conn != NULL);
    return conn->contentLength;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContent --
 *
 *      Return pointer to start of content.
 *
 * Results:
 *      Start of content.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnContent(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    return connPtr->reqPtr->content;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentSize --
 *
 *      Return size of the posted content.
 *
 * Results:
 *      Size of the content buffer
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_ConnContentSize(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->reqPtr->length;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentFile --
 *
 *      Return pointer of the file name with spooled content.
 *
 * Results:
 *      Pointer to string
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnContentFile(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;
    
    assert(conn != NULL);
    return connPtr->sockPtr != NULL ? connPtr->sockPtr->tfile : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentFd --
 *
 *      Return opened file descriptor of the file with spooled content.
 *
 * Results:
 *      File descriptor or 0 if not used
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnContentFd(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->sockPtr != NULL ? connPtr->sockPtr->tfd : 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnServer --
 *
 *      Get the server name
 *
 * Results:
 *      A string ptr to the server name
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnServer(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->server;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnResponseStatus, Ns_ConnSetResponseStatus --
 *
 *      Get (set) the HTTP reponse code that will be sent.
 *
 * Results:
 *      An integer response code (e.g., 200 for OK).
 *
 * Side effects:
 *      NB: Status 200 is the default and can not be set manualy.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnResponseStatus(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->responseStatus;

}

void
Ns_ConnSetResponseStatus(Ns_Conn *conn, int newStatus)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    if (newStatus != 200) {
        connPtr->responseStatus = newStatus;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentSent --
 *
 *      Return the number of bytes sent to the browser after headers
 *
 * Results:
 *      Bytes sent
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_ConnContentSent(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;
    
    assert(conn != NULL);
    return connPtr->nContentSent;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetContentSent --
 *
 *      Set the number of bytes sent to the browser after headers
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnSetContentSent(Ns_Conn *conn, size_t length)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    connPtr->nContentSent = length;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnResponseLength --
 *
 *      Get the response length
 *
 * Results:
 *      Integer, number of bytes to send
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_ConnResponseLength(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->responseLength;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnPeer --
 *
 *      Get the peer's internet address
 *
 * Results:
 *      A string IP address
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnPeer(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->reqPtr->peer;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetPeer --
 *
 *      Set the peer's internet address and port
 *
 * Results:
 *      A string IP address
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConnSetPeer(Ns_Conn *conn, const struct sockaddr_in *saPtr)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    assert(saPtr != NULL);

    connPtr->reqPtr->port = (int)ntohs(saPtr->sin_port);
    strncpy(connPtr->reqPtr->peer, ns_inet_ntoa(saPtr->sin_addr), NS_IPADDR_SIZE);
    
    return connPtr->reqPtr->peer;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnPeerPort --
 *
 *      Get the port from which the peer is coming
 *
 * Results:
 *      The port number.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnPeerPort(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->reqPtr->port;
}


/*
 *----------------------------------------------------------------------
 * Ns_SetConnLocationProc --
 *
 *      Set pointer to custom routine that acts like
 *      Ns_ConnLocationAppend();
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Overrides an old-style Ns_LocationProc.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetConnLocationProc(Ns_ConnLocationProc *proc, void *arg)
{
    NsServer *servPtr = NsGetInitServer();

    assert(proc != NULL);
    assert(arg != NULL);
    
    if (servPtr == NULL) {
        Ns_Log(Error, "Ns_SetConnLocationProc: no initializing server");
        return NS_ERROR;
    }

    servPtr->vhost.connLocationProc = proc;
    servPtr->vhost.connLocationArg = arg;

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 * Ns_SetLocationProc --
 *
 *      Set pointer to custom routine that acts like Ns_ConnLocation();
 *
 *      Deprecated: Use Ns_SetConnLocationProc() which is virtual host
 *      aware.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetLocationProc(const char *server, Ns_LocationProc *proc)
{
    NsServer *servPtr = NsGetServer(server);

    assert(server != NULL);
    assert(proc != NULL);
    
    if (servPtr != NULL) {
        servPtr->vhost.locationProc = proc;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnLocation --
 *
 *      Get the location according to the driver for this connection.
 *      It is of the form SCHEME://HOSTNAME:PORT
 *
 *      Deprecated: Use Ns_ConnLocationAppend() which is virtual host
 *      aware.
 *
 * Results:
 *      A string URL, not including path
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnLocation(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;
    NsServer *servPtr = connPtr->poolPtr->servPtr;
    const char *location = NULL;

    if (servPtr->vhost.locationProc != NULL) {
        location = (*servPtr->vhost.locationProc)(conn);
    }
    if (location == NULL) {
        location = connPtr->location;
    }

    return location;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnLocationAppend --
 *
 *      Append the location of this connection to dest. It is of the
 *      form SCHEME://HOSTNAME:PORT
 *
 * Results:
 *      dest->string.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConnLocationAppend(Ns_Conn *conn, Ns_DString *dest)
{
    Conn       *connPtr = (Conn *) conn;
    NsServer   *servPtr;
    Ns_Set     *headers;
    char       *location, *host;

    assert(conn != NULL);
    assert(dest != NULL);
    
    assert(connPtr->poolPtr != NULL);
    servPtr = connPtr->poolPtr->servPtr;
    assert(servPtr != NULL);
    
    if (servPtr->vhost.connLocationProc != NULL) {

        /*
         * Prefer the new style Ns_ConnLocationProc.
         */

        location = (*servPtr->vhost.connLocationProc)
            (conn, dest, servPtr->vhost.connLocationArg);
        if (location == NULL) {
            goto deflocation;
        }

    } else if (servPtr->vhost.locationProc != NULL) {

        /*
         * Fall back to old style Ns_LocationProc.
         */

        location = (*servPtr->vhost.locationProc)(conn);
        if (location == NULL) {
            goto deflocation;
        }
        location = Ns_DStringAppend(dest, location);

    } else if (servPtr->vhost.enabled != 0
               && (headers = Ns_ConnHeaders(conn)) != NULL
               && (host = Ns_SetIGet(headers, "Host")) != NULL
               && *host != '\0') {

        /*
         * Construct a location string from the HTTP host header.
         */

        if (Ns_StrIsHost(host) == 0) {
            goto deflocation;
        }

        location = Ns_DStringVarAppend(dest,
            connPtr->drvPtr->protocol, "://", host, NULL);

    } else {

        /*
         * If all else fails, append the static driver location.
         */

    deflocation:
        location = Ns_DStringAppend(dest, connPtr->location);
    }

    return location;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnHost --
 *
 *      Get the address of the current connection
 *
 * Results:
 *      A string address
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnHost(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    assert(connPtr->drvPtr != NULL);
    return connPtr->drvPtr->address;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnPort --
 *
 *      What server port is this connection on?
 *
 * Results:
 *      Integer port number
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnPort(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    assert(connPtr->drvPtr != NULL);
    return connPtr->drvPtr->port;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSock --
 *
 *      Return the underlying socket for a connection.
 *
 * Results:
 *      socket descriptor
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_ConnSock(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;
    
    assert(conn != NULL);
    return (connPtr->sockPtr != NULL ? connPtr->sockPtr->sock : NS_INVALID_SOCKET);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSockPtr --
 *
 *      Return the underlying socket for a connection.
 *
 * Results:
 *      Ns_sock struct
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Ns_Sock *
Ns_ConnSockPtr(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return (Ns_Sock *)connPtr->sockPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSockContent --
 *
 *      Returns read buffer for incoming requests
 *
 * Results:
 *      NULL if no content have been read yet
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Ns_DString *
Ns_ConnSockContent(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *)conn;

    assert(conn != NULL);
    if (connPtr->reqPtr != NULL) {
        return &connPtr->reqPtr->buffer;
    }
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnDriverName --
 *
 *      Return the name of this driver
 *
 * Results:
 *      A driver name
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnDriverName(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->drvPtr->name;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnStartTime --
 *
 *      Return the connection start time, which is the time the
 *      connection was queued from the driver thread, not the time the
 *      underlying socket was opened to the server. Similarly
 *      Ns_ConnAcceptTime() returns the time the connection was
 *      accepted (this is maybe a kept open connection),
 *      Ns_ConnQueueTime() returns the time a request was queued,
 *      Ns_ConnDequeueTime() returns the time a request was taken out
 *      of the queue, and Ns_ConnFilterTime() is the time stampt after
 *      the filters are executed.
 *
 * Results:
 *      Ns_Time pointer.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

Ns_Time *
Ns_ConnStartTime(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    return &connPtr->requestQueueTime;
}

Ns_Time *
Ns_ConnAcceptTime(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    return &connPtr->acceptTime;
}

Ns_Time *
Ns_ConnQueueTime(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    return &connPtr->requestQueueTime;
}

Ns_Time *
Ns_ConnDequeueTime(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    return &connPtr->requestDequeueTime;
}

Ns_Time *
Ns_ConnFilterTime(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    return &connPtr->filterDoneTime;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnTimeStats --
 *
 *      Return for a given connection the time spans computed by
 *      Ns_ConnTimeStats()
 *      
 * Results:
 *      Four time structures (argument 2 to 5)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Ns_ConnTimeSpans(const Ns_Conn *conn, 
		 Ns_Time *acceptTimeSpanPtr, Ns_Time *queueTimeSpanPtr, 
		 Ns_Time *filterTimeSpanPtr, Ns_Time *runTimeSpanPtr) {
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    assert(acceptTimeSpanPtr != NULL);
    assert(queueTimeSpanPtr != NULL);
    assert(filterTimeSpanPtr != NULL);
    assert(runTimeSpanPtr != NULL);

    *acceptTimeSpanPtr = connPtr->acceptTimeSpan;
    *queueTimeSpanPtr  = connPtr->queueTimeSpan;
    *filterTimeSpanPtr = connPtr->filterTimeSpan;
    *runTimeSpanPtr    = connPtr->runTimeSpan;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnTimeStats --
 *
 *      Compute for a given connection various time spans such as
 *      acceptTimeSpan, queueTimeSpan, filterTimeSpan and
 *      runTimeSpan as follows
 *      
 *         acceptTimeSpan = queueTime - acceptTime 
 *         queueTimeSpan  = dequeueTime - queueTime
 *         filterTimeSpan = filterDoneTime - dequeueTime
 *         runTimeSpan    = now - filterDoneTime
 *
 *      In addition, this function updates the statistics and should
 *      be called only once per request.
 *
 * Results:
 *      none
 *
 * Side effects:
 *      update statistics
 *
 *----------------------------------------------------------------------
 */
void
Ns_ConnTimeStats(Ns_Conn *conn) {
    Conn *connPtr = (Conn *) conn;
    ConnPool   *poolPtr;
    Ns_Time     now;

    assert(conn != NULL);
    
    poolPtr = connPtr->poolPtr;
    assert(poolPtr != NULL);
    
    Ns_GetTime(&now);

    Ns_DiffTime(&connPtr->requestQueueTime,   &connPtr->acceptTime,         &connPtr->acceptTimeSpan);
    Ns_DiffTime(&connPtr->requestDequeueTime, &connPtr->requestQueueTime,   &connPtr->queueTimeSpan);
    Ns_DiffTime(&connPtr->filterDoneTime,     &connPtr->requestDequeueTime, &connPtr->filterTimeSpan);
    Ns_DiffTime(&now,                         &connPtr->filterDoneTime,     &connPtr->runTimeSpan);

    Ns_MutexLock(&poolPtr->threads.lock);
    Ns_IncrTime(&poolPtr->stats.acceptTime, connPtr->acceptTimeSpan.sec, connPtr->acceptTimeSpan.usec);
    Ns_IncrTime(&poolPtr->stats.queueTime,  connPtr->queueTimeSpan.sec,  connPtr->queueTimeSpan.usec);
    Ns_IncrTime(&poolPtr->stats.filterTime, connPtr->filterTimeSpan.sec, connPtr->filterTimeSpan.usec);
    Ns_IncrTime(&poolPtr->stats.runTime,    connPtr->runTimeSpan.sec,    connPtr->runTimeSpan.usec);
    Ns_MutexUnlock(&poolPtr->threads.lock);
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnTimeout --
 *
 *      Absolute time value beyond which conn should not wait on
 *      resources, such as condition variables.
 *
 * Results:
 *      Ns_Time pointer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Time *
Ns_ConnTimeout(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    return &connPtr->timeout;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnId --
 *
 *      Return the connection id.
 *
 * Results:
 *      The connection id.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uintptr_t
Ns_ConnId(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;
    assert(conn != NULL);
    
    return connPtr->id;
}

const char *
NsConnIdStr(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;
    assert(conn != NULL);
    
    return connPtr->idstr;
}




/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnModifiedSince --
 *
 *      Has the data the url points to changed since a given time?
 *
 * Results:
 *      NS_TRUE if data modified, NS_FALSE otherwise.
 *
 * Side effects:
 *      None
 *
 * NOTE: This doesn't do a strict time check.  If the server flags aren't
 *       set to check modification, or if there wasn't an 'If-Modified-Since'
 *       header in the request, then this'll always return true.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_ConnModifiedSince(const Ns_Conn *conn, time_t since)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);

    if (connPtr->poolPtr->servPtr->opts.modsince != 0) {
	char *hdr = Ns_SetIGet(conn->headers, "If-Modified-Since");
        if (hdr != NULL && Ns_ParseHttpTime(hdr) >= since) {
            return NS_FALSE;
        }
    }
    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnUnmodifiedSince --
 *
 *      Has the data the url points to changed since a given time?
 *
 * Results:
 *      NS_TRUE if data unmodified or header not present, NS_FALSE otherwise.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

bool
Ns_ConnUnmodifiedSince(const Ns_Conn *conn, time_t since)
{
    char *hdr;

    hdr = Ns_SetIGet(conn->headers, "If-Unmodified-Since");
    if (hdr != NULL && Ns_ParseHttpTime(hdr) < since) {
        return NS_FALSE;
    }
    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGetEncoding, Ns_ConnSetEncoding --
 *
 *      Get (set) the Tcl_Encoding for the connection which is used
 *      to convert from UTF to specified output character set.
 *
 * Results:
 *      Pointer to Tcl_Encoding (get) or NULL (set).
 *
 * Side effects:
 *      See Ns_ConnGetQuery().
 *
 *----------------------------------------------------------------------
 */

Tcl_Encoding
Ns_ConnGetEncoding(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->outputEncoding;
}

void
Ns_ConnSetEncoding(Ns_Conn *conn, Tcl_Encoding encoding)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    connPtr->outputEncoding = encoding;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGetUrlEncoding, Ns_ConnSetUrlEncoding --
 *
 *      Get (set) the Tcl_Encoding for the connection which is used
 *      to convert input forms to proper UTF.
 *
 * Results:
 *      Pointer to Tcl_Encoding (get) or NULL (set).
 *
 * Side effects:
 *      See Ns_ConnGetQuery().
 *
 *----------------------------------------------------------------------
 */

Tcl_Encoding
Ns_ConnGetUrlEncoding(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->urlEncoding;
}

void
Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);
    connPtr->urlEncoding = encoding;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGetCompression, Ns_ConnSetCompression
 *
 *      Enable/disable compression wth the specified level.
 *      Output will be compressed if client advertises support.
 *
 *      Level 1 is 'on' i.e. default compression from config.
 *
 * Results:
 *      Compression level, 0-9.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnGetCompression(const Ns_Conn *conn)
{
    const Conn *connPtr = (const Conn *) conn;

    assert(conn != NULL);
    return connPtr->requestCompress;
}

void
Ns_ConnSetCompression(Ns_Conn *conn, int level)
{
    Conn *connPtr = (Conn *) conn;

    assert(conn != NULL);

#ifdef HAVE_ZLIB_H
    connPtr->requestCompress = MIN(MAX(level, 0), 9);
#else
    connPtr->requestCompress = 0;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConnObjCmd --
 *
 *      Implements ns_conn as an obj command.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclConnObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp *itPtr = clientData;
    Ns_Conn *conn = itPtr->conn;
    Conn *connPtr = (Conn *) conn;
    Ns_Request *request;
    Tcl_Encoding encoding;
    Tcl_Channel chan;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    FormFile *filePtr;
    Ns_DString ds;
    int idx, off, len, opt, n;
    const char *setName;
    int         setNameLength;

    static const char *opts[] = {
	"auth", "authpassword", "authuser", 
	"channel", "clientdata", "close", "compress", "content", 
	"contentfile", "contentlength", "contentsentlength", "copy", 
	"driver", 
	"encoding", 
	"fileheaders", "filelength", "fileoffset","files", "flags", "form", 
	"headers", "host", 
	"id", "isconnected", 
	"keepalive", 
	"location", 
	"method",
	"outputheaders", 
	"peeraddr", "peerport", "pool", "port", "protocol",
	"query", 
	"request", 
	"server", "sock", "start", "status", 
	"timeout",
	"url", "urlc", "urlencoding", "urlv",
	"version",
	"zipaccepted",
        NULL
    };
    enum ISubCmdIdx {
	CAuthIdx, CAuthPasswordIdx, CAuthUserIdx, 
	CChannelIdx, CClientdataIdx, CCloseIdx, CCompressIdx, CContentIdx, 
	CContentFileIdx, CContentLengthIdx, CContentSentLenIdx, CCopyIdx, 
	CDriverIdx, 
	CEncodingIdx,
	CFileHdrIdx, CFileLenIdx, CFileOffIdx, CFilesIdx, CFlagsIdx, CFormIdx, 
	CHeadersIdx, CHostIdx, 
	CIdIdx, CIsConnectedIdx,
	CKeepAliveIdx, 
	CLocationIdx, 
	CMethodIdx, 
	COutputHeadersIdx, 
	CPeerAddrIdx, CPeerPortIdx, CPoolIdx, CPortIdx, CProtocolIdx, 
	CQueryIdx, 
	CRequestIdx,
	CServerIdx, CSockIdx, CStartIdx, CStatusIdx, 
	CTimeoutIdx, 
	CUrlIdx, CUrlcIdx, CUrlEncodingIdx, CUrlvIdx, 
	CVersionIdx,
	CZipacceptedIdx
    };

    if (unlikely(objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "option");
        return TCL_ERROR;
    }
    if (unlikely(Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
				     &opt) != TCL_OK)) {
        return TCL_ERROR;
    }

    /*
     * Only the "isconnected" option operates without a conn.
     */

    if (unlikely(opt == (int)CIsConnectedIdx)) {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj((connPtr != NULL) ? 1 : 0));
        return TCL_OK;
    }
    if (unlikely(connPtr == NULL)) {
        Tcl_SetResult(interp, "no current connection", TCL_STATIC);
        return TCL_ERROR;
    }

    request = connPtr->request;
    switch (opt) {

    case CIsConnectedIdx:
        /* NB: Not reached - silence compiler warning. */
        break;

    case CKeepAliveIdx:
        if (objc > 2 && Tcl_GetIntFromObj(interp, objv[2],
                                          &connPtr->keep) != TCL_OK) {
            return NS_ERROR;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(connPtr->keep));
        break;

    case CClientdataIdx:
        if (objc > 2) {
	    const char *value = Tcl_GetString(objv[2]);
	    if (connPtr->clientData != NULL) {
		ns_free(connPtr->clientData);
	    }
	    connPtr->clientData = ns_strdup(value);
	}
	Tcl_SetObjResult(interp, Tcl_NewStringObj(connPtr->clientData, -1));
        break;

    case CCompressIdx:
        if (objc > 2) {
            if (Tcl_GetIntFromObj(interp, objv[2], &n) != TCL_OK
                    && Tcl_GetBooleanFromObj(interp, objv[2], &n) != TCL_OK) {
                return NS_ERROR;
            }
            Ns_ConnSetCompression(conn, n);
        }
        Tcl_SetObjResult(interp,
                         Tcl_NewIntObj(Ns_ConnGetCompression(conn)));
        break;

    case CUrlvIdx:
        if (objc == 2) {
            for (idx = 0; idx < request->urlc; idx++) {
                Tcl_AppendElement(interp, request->urlv[idx]);
            }
        } else if (Tcl_GetIntFromObj(interp, objv[2], &idx) != TCL_OK) {
            return TCL_ERROR;
        } else if (idx >= 0 && idx < request->urlc) {
            Tcl_SetResult(interp, request->urlv[idx], TCL_STATIC);
        }
        break;

    case CAuthIdx:
        if ((itPtr->nsconn.flags & CONN_TCLAUTH) != 0U) {
            Tcl_SetResult(interp, itPtr->nsconn.auth, TCL_STATIC);
        } else {
            if (connPtr->auth == NULL) {
                connPtr->auth = Ns_SetCreate(NULL);
            }
            Ns_TclEnterSet(interp, connPtr->auth, NS_TCL_SET_STATIC);
            setName = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &setNameLength);
            setNameLength++;
            memcpy(itPtr->nsconn.auth, setName, MIN((size_t)setNameLength, NS_SET_SIZE));
            itPtr->nsconn.flags |= CONN_TCLAUTH;
        }
        break;

    case CAuthUserIdx:
        if (connPtr->auth != NULL) {
            Tcl_AppendResult(interp, Ns_ConnAuthUser(conn), NULL);
        }
        break;

    case CAuthPasswordIdx:
        if (connPtr->auth != NULL) {
            Tcl_AppendResult(interp, Ns_ConnAuthPasswd(conn), NULL);
        }
        break;

    case CContentIdx:
        {
            bool        binary = NS_FALSE;
            int         offset = 0, length = -1, requiredLength;
            size_t      contentLength;
            char       *content;
            Tcl_DString encDs;
            
            Ns_ObjvSpec lopts[] = {
                {"-binary",    Ns_ObjvBool,  &binary, INT2PTR(NS_TRUE)},
                {NULL, NULL, NULL, NULL}
            };
            Ns_ObjvSpec args[] = {
                {"?offset", Ns_ObjvInt, &offset, NULL},
                {"?length", Ns_ObjvInt, &length, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
                return TCL_ERROR;
            }

            if ((connPtr->flags & NS_CONN_CLOSED) != 0U) {
                /* 
                 * In cases, the content is allocated via mmap, the content
                 * is unmapped when the socket is closed. Accessing the
                 * content will crash the server. Although we might not have
                 * the same problem when the content is allocated
                 * differently, we use here the restrictive strategy to
                 * provide consistant behavior independent of the allocation
                 * strategy.
                 */
                Tcl_AppendResult(interp, "connection already closed, can't get content", NULL);
                return TCL_ERROR;
            }
            if (offset < 0 || length < -1) {
                Tcl_AppendResult(interp, "invalid offset and/or length specified", NULL);
                return TCL_ERROR;
            }
            
            requiredLength = length;
            if (offset > 0 && ((size_t)offset > connPtr->reqPtr->length)) {
                Tcl_AppendResult(interp, "offset exceeds available content length", NULL);
                return TCL_ERROR;
            }

            if (length == -1) {
                length = (int)connPtr->reqPtr->length - offset;
            } else if (length > -1 && ((size_t)length + (size_t)offset > connPtr->reqPtr->length)) {
                Tcl_AppendResult(interp, "offset + length exceeds available content length", NULL);
                return TCL_ERROR;
            }
                        
            if (connPtr->reqPtr->length == 0u) {
                content = NULL;
                contentLength = 0u;
                Tcl_ResetResult(interp);
            } else if (binary == NS_FALSE) {
                content = Tcl_ExternalToUtfDString(connPtr->outputEncoding,
                                                   connPtr->reqPtr->content,
                                                   (int)connPtr->reqPtr->length,
                                                   &encDs);
                contentLength = (size_t)Tcl_DStringLength(&encDs);
                if (requiredLength == -1) {
                    length = Tcl_DStringLength(&encDs) - offset;
                }
            } else {
                content = connPtr->reqPtr->content;
                contentLength = connPtr->reqPtr->length;
            }

            if (contentLength > 0) {
                if (requiredLength == -1 && offset == 0) {
                    /*
                     * return full content
                     */
                    if (binary == NS_FALSE) {
                        Tcl_DStringResult(interp, &encDs);
                    } else {
                        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((uint8_t*)connPtr->reqPtr->content, 
                                                                     (int)connPtr->reqPtr->length));
                    }
                } else {
                    /*
                     * return partial content
                     */
                    if (binary == NS_FALSE) {
                        Tcl_Obj *contentObj = Tcl_NewStringObj(content, (int)contentLength);
                        
                        Tcl_SetObjResult(interp, Tcl_GetRange(contentObj, offset, offset+length-1));
                        Tcl_DStringFree(&encDs);
                        Tcl_DecrRefCount(contentObj);
                    } else {
                        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((const uint8_t*)content + offset, 
                                                                     (int)length));
                    }
                }
            }
            break;
        }

    case CContentLengthIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)conn->contentLength));
        break;

    case CContentFileIdx:
        Tcl_AppendResult(interp, Ns_ConnContentFile(conn), NULL);
        break;

    case CEncodingIdx:
        if (objc > 2) {
            encoding = Ns_GetCharsetEncoding(Tcl_GetString(objv[2]));
            if (encoding == NULL) {
                Tcl_AppendResult(interp, "no such encoding: ",
                                 Tcl_GetString(objv[2]), NULL);
                return TCL_ERROR;
            }
            connPtr->outputEncoding = encoding;
        }
        if (connPtr->outputEncoding != NULL) {
            const char *charset = Ns_GetEncodingCharset(connPtr->outputEncoding);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(charset, -1));
        }
        break;

    case CUrlEncodingIdx:
        if (objc > 2) {
            encoding = Ns_GetCharsetEncoding(Tcl_GetString(objv[2]));
            if (encoding == NULL) {
                Tcl_AppendResult(interp, "no such encoding: ",
                                 Tcl_GetString(objv[2]), NULL);
                return TCL_ERROR;
            }
            /*
             * Check to see if form data has already been parsed.
             * If so, and the urlEncoding is changing, then clear
             * the previous form data.
             */
            if ((connPtr->urlEncoding != encoding)
                && (itPtr->nsconn.flags & CONN_TCLFORM) != 0U) {

                Ns_ConnClearQuery(conn);
                itPtr->nsconn.flags ^= CONN_TCLFORM;
            }
            connPtr->urlEncoding = encoding;
        }
        if (connPtr->urlEncoding != NULL) {
            const char *charset = Ns_GetEncodingCharset(connPtr->urlEncoding);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(charset, -1));
        }
        break;

    case CPeerAddrIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_ConnPeer(conn), -1));
        break;

    case CPeerPortIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_ConnPeerPort(conn)));
        break;

    case CHeadersIdx:
        if (likely((itPtr->nsconn.flags & CONN_TCLHDRS) != 0U)) {
            Tcl_SetResult(interp, itPtr->nsconn.hdrs, TCL_STATIC);
        } else {
            Ns_TclEnterSet(interp, connPtr->headers, NS_TCL_SET_STATIC);
            setName = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &setNameLength);
            setNameLength++;
            memcpy(itPtr->nsconn.hdrs, setName, MIN((size_t)setNameLength, NS_SET_SIZE));
            itPtr->nsconn.flags |= CONN_TCLHDRS;
        }
        break;

    case COutputHeadersIdx:
        if (likely((itPtr->nsconn.flags & CONN_TCLOUTHDRS) != 0U)) {
            Tcl_SetResult(interp, itPtr->nsconn.outhdrs, TCL_STATIC);
        } else {
            Ns_TclEnterSet(interp, connPtr->outputheaders, NS_TCL_SET_STATIC);
            setName = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &setNameLength);
            setNameLength++;
            memcpy(itPtr->nsconn.outhdrs, setName, MIN((size_t)setNameLength, NS_SET_SIZE));
            itPtr->nsconn.flags |= CONN_TCLOUTHDRS;
        }
        break;

    case CFormIdx:
	if ((itPtr->nsconn.flags & CONN_TCLFORM) != 0U) {
            Tcl_SetResult(interp, itPtr->nsconn.form, TCL_STATIC);
        } else {
	    Ns_Set *form = Ns_ConnGetQuery(conn);

            if (form == NULL) {
                itPtr->nsconn.form[0] = '\0';
            } else {
                Ns_TclEnterSet(interp, form, NS_TCL_SET_STATIC);
                setName = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &setNameLength);
                setNameLength++;
                memcpy(itPtr->nsconn.form, setName, MIN((size_t)setNameLength, NS_SET_SIZE));
            }
            itPtr->nsconn.flags |= CONN_TCLFORM;
        }
        break;

    case CFilesIdx:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        hPtr = Tcl_FirstHashEntry(&connPtr->files, &search);
        while (hPtr != NULL) {
            Tcl_AppendElement(interp, Tcl_GetHashKey(&connPtr->files, hPtr));
            hPtr = Tcl_NextHashEntry(&search);
        }
        break;

    case CFileOffIdx: /* fall through */
    case CFileLenIdx: /* fall through */
    case CFileHdrIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        hPtr = Tcl_FindHashEntry(&connPtr->files, Tcl_GetString(objv[2]));
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "no such file: ", Tcl_GetString(objv[2]),
                             NULL);
            return TCL_ERROR;
        }
        filePtr = Tcl_GetHashValue(hPtr);
        if (opt == (int)CFileOffIdx) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj((int)filePtr->off));
        } else if (opt == (int)CFileLenIdx) {
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)filePtr->len));
        } else {
            Ns_TclEnterSet(interp, filePtr->hdrs, NS_TCL_SET_STATIC);
        }
        break;

    case CCopyIdx:
        if (objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "off len chan");
            return TCL_ERROR;
        }
        if (GetIndices(interp, connPtr, objv+2, &off, &len) != TCL_OK ||
            GetChan(interp, Tcl_GetString(objv[4]), &chan) != TCL_OK) {
            return TCL_ERROR;
        }
        if (Tcl_Write(chan, connPtr->reqPtr->content + off, len) != len) {
            Tcl_AppendResult(interp, "could not write ",
                             Tcl_GetString(objv[3]), " bytes to ",
                             Tcl_GetString(objv[4]), ": ",
                             Tcl_PosixError(interp), NULL);
            return TCL_ERROR;
        }
        break;

    case CRequestIdx:
	Tcl_SetObjResult(interp, Tcl_NewStringObj(request->line, -1));
        break;

    case CMethodIdx:
	Tcl_SetObjResult(interp, Tcl_NewStringObj(request->method, -1));
        break;

    case CProtocolIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(connPtr->drvPtr->protocol, -1));
        break;

    case CHostIdx:
	Tcl_SetObjResult(interp, Tcl_NewStringObj(request->host, -1));
        break;

    case CPortIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj((int)request->port));
        break;

    case CUrlIdx:
	Tcl_SetObjResult(interp, Tcl_NewStringObj(request->url, -1));
        break;

    case CQueryIdx:
	Tcl_SetObjResult(interp, Tcl_NewStringObj(request->query, -1));
        break;

    case CUrlcIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(request->urlc));
        break;

    case CVersionIdx:
        Tcl_SetObjResult(interp, Tcl_NewDoubleObj(request->version));
        break;

    case CLocationIdx:
        Ns_DStringInit(&ds);
        (void) Ns_ConnLocationAppend(conn, &ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case CDriverIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_ConnDriverName(conn), -1));
        break;

    case CServerIdx:
	Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_ConnServer(conn), -1));
        break;

    case CPoolIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(connPtr->poolPtr->pool, -1));
        break;

    case CStatusIdx:
        if (objc < 2 || objc > 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "?status?");
            return TCL_ERROR;
        } else if (objc == 3) {
            int status;
            if (Tcl_GetIntFromObj(interp, objv[2], &status) != TCL_OK) {
                return TCL_ERROR;
            }
            Tcl_SetObjResult(interp,Tcl_NewIntObj(Ns_ConnResponseStatus(conn)));
            Ns_ConnSetResponseStatus(conn, status);
        } else {
            Tcl_SetObjResult(interp,Tcl_NewIntObj(Ns_ConnResponseStatus(conn)));
        }
        break;

    case CTimeoutIdx:
        Tcl_SetObjResult(interp, Ns_TclNewTimeObj(Ns_ConnTimeout(conn)));
        break;

    case CSockIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj((int)Ns_ConnSock(conn)));
        break;

    case CIdIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)Ns_ConnId(conn)));
        break;

    case CFlagsIdx:
	Tcl_SetObjResult(interp, Tcl_NewIntObj((int)connPtr->flags));
        break;

    case CStartIdx:
        Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&connPtr->requestQueueTime));
        break;

    case CCloseIdx:
	(void) Ns_ConnClose(conn);
        break;

    case CChannelIdx:
        chan = MakeConnChannel(itPtr, conn);
        if (chan == NULL) {
            return TCL_ERROR;
        }
        Tcl_RegisterChannel(interp, chan);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(chan),-1));
	break;

    case CContentSentLenIdx:
        if (objc == 2) {
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)connPtr->nContentSent));
        } else if (objc == 3) {
	    Tcl_WideInt sent;
            if (Tcl_GetWideIntFromObj(interp, objv[2], &sent) != TCL_OK) {
                return TCL_ERROR;
            }
	    connPtr->nContentSent = (size_t)sent;
        } else {
            Tcl_WrongNumArgs(interp, 2, objv, "?value?");
            return TCL_ERROR;
        }
	break;

    case CZipacceptedIdx:
	Tcl_SetObjResult(interp, Tcl_NewBooleanObj((connPtr->flags & NS_CONN_ZIPACCEPTED) != 0U));
	break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;

    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLocationProcObjCmd --
 *
 *      Implements ns_locationproc as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclLocationProcObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsServer *servPtr = NsGetInitServer();
    Ns_TclCallback *cbPtr;
    int result;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        return TCL_ERROR;
    }
    if (servPtr == NULL) {
        Tcl_AppendResult(interp, "no initializing server", TCL_STATIC);
        return TCL_ERROR;
    }
    cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *)NsTclConnLocation, 
			      objv[1], objc - 2, objv + 2);
    result = Ns_SetConnLocationProc(NsTclConnLocation, cbPtr);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclWriteContentObjCmd --
 *
 *      Implements ns_conncptofp as obj command.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclWriteContentObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    int          toCopy = 0;
    const char  *chanName;
    Request     *reqPtr;
    Tcl_Channel  chan;

    /*
     * Syntax: ns_conncptofp ?-bytes tocopy? channel
     */

    Ns_ObjvSpec opts[] = {
        {"-bytes",   Ns_ObjvInt,   &toCopy, NULL},
        {"--",       Ns_ObjvBreak, NULL,    NULL},
        {NULL,       NULL,         NULL,    NULL}
    };
    Ns_ObjvSpec args[] = {
        {"channel",  Ns_ObjvString, &chanName, NULL},
        {NULL,       NULL,          NULL,      NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (itPtr->conn == NULL) {
        Tcl_SetResult(interp, "no connection", TCL_STATIC);
        return TCL_ERROR;
    }
    if (GetChan(interp, chanName, &chan) != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_Flush(chan) != TCL_OK) {
	Ns_TclPrintfResult(interp, "flush returned error: %s", strerror(Tcl_GetErrno()));
	return TCL_ERROR;
    }
    reqPtr = ((Conn *)itPtr->conn)->reqPtr;
    if (toCopy > (int)reqPtr->avail || toCopy <= 0) {
        toCopy = (int)reqPtr->avail;
    }
    if (Ns_ConnCopyToChannel(itPtr->conn, (size_t)toCopy, chan) != NS_OK) {
        Tcl_SetResult(interp, "could not copy content", TCL_STATIC);
        return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConnLocation --
 *
 *      Tcl callback to construct location string.
 *
 * Results:
 *      dest->string or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
NsTclConnLocation(Ns_Conn *conn, Ns_DString *dest, void *arg)
{
    Ns_TclCallback *cbPtr = arg;
    Tcl_Interp *interp = Ns_GetConnInterp(conn);

    if (Ns_TclEvalCallback(interp, cbPtr, dest, (char *)0) != NS_OK) {
	(void) Ns_TclLogErrorInfo(interp, "\n(context: location callback)");
        return NULL;
    }

    return Ns_DStringValue(dest);
}


/*
 *----------------------------------------------------------------------
 *
 * GetChan --
 *
 *      Return an open channel.
 *
 * Results:
 *      TCL_OK if given a valid channel id, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Channel is set in given chanPtr or error message left in
 *      given interp.
 *
 *----------------------------------------------------------------------
 */

static int
GetChan(Tcl_Interp *interp, const char *id, Tcl_Channel *chanPtr)
{
    Tcl_Channel chan;
    int mode;

    assert(interp != NULL);
    assert(id != NULL);
    assert(chanPtr != NULL);

    chan = Tcl_GetChannel(interp, id, &mode);
    if (chan == (Tcl_Channel) NULL) {
        return TCL_ERROR;
    }
    if ((mode & TCL_WRITABLE) == 0) {
        Tcl_AppendResult(interp, "channel \"", id,
                         "\" wasn't opened for writing", NULL);
        return TCL_ERROR;
    }

    *chanPtr = chan;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * GetIndices --
 *
 *      Return offset and length from given Tcl_Obj's.
 *
 * Results:
 *      TCL_OK if objects are valid offsets, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Given offPtr and lenPtr are updated with indices or error
 *      message is left in the interp.
 *
 *----------------------------------------------------------------------
 */

static int
GetIndices(Tcl_Interp *interp, const Conn *connPtr, Tcl_Obj *CONST* objv, int *offPtr,
           int *lenPtr)
{
    int off, len;

    assert(interp != NULL);
    assert(connPtr != NULL);

    if (Tcl_GetIntFromObj(interp, objv[0], &off) != TCL_OK
        || Tcl_GetIntFromObj(interp, objv[1], &len) != TCL_OK) {
        return TCL_ERROR;
    }
    if (off < 0 || (size_t)off > connPtr->reqPtr->length) {
        Tcl_AppendResult(interp, "invalid offset: ", Tcl_GetString(objv[0]),
                         NULL);
        return TCL_ERROR;
    }
    if (len < 0 || (size_t)len > (connPtr->reqPtr->length - (size_t)off)) {
        Tcl_AppendResult(interp, "invalid length: ", Tcl_GetString(objv[1]),
                         NULL);
        return TCL_ERROR;
    }

    *offPtr = off;
    *lenPtr = len;

    return TCL_OK;
}


/*----------------------------------------------------------------------------
 *
 * MakeConnChannel --
 *
 *      Wraps a Tcl channel arround the current connection socket
 *      and returns the channel handle to the caller.
 *
 * Result:
 *      Tcl_Channel handle or NULL.
 *
 * Side Effects:
 *      Removes the socket from the connection structure.
 *
 *----------------------------------------------------------------------------
 */

static Tcl_Channel
MakeConnChannel(const NsInterp *itPtr, Ns_Conn *conn)
{
    Conn       *connPtr = (Conn *) conn;
    Tcl_Channel chan;

    assert(conn != NULL);
    assert(itPtr != NULL);

    if ((connPtr->flags & NS_CONN_CLOSED) != 0U) {
        Tcl_AppendResult(itPtr->interp, "connection closed", NULL);
        return NULL;
    }

    if (connPtr->sockPtr->sock == NS_INVALID_SOCKET) {
        Tcl_AppendResult(itPtr->interp, "no socket for connection", NULL);
        return NULL;
    }

    /*
     * Create Tcl channel arround the connection socket
     */

    chan = Tcl_MakeTcpClientChannel(NSSOCK2PTR(connPtr->sockPtr->sock));
    if (chan == NULL) {
        Tcl_AppendResult(itPtr->interp, Tcl_PosixError(itPtr->interp), NULL);
        return NULL;
    }

    /*
     * Disable keep-alive and chunking headers.
     */

    if (connPtr->responseLength < 0) {
        connPtr->keep = NS_FALSE;
    }

    /*
     * Check to see if HTTP headers are required and flush
     * them now before the conn socket is dissociated.
     */

    if ((conn->flags & NS_CONN_SENTHDRS) == 0U) {
        if ((itPtr->nsconn.flags & CONN_TCLHTTP) == 0U) {
            conn->flags |= NS_CONN_SKIPHDRS;
        } else {
            if (Ns_ConnWriteVData(conn, NULL, 0, NS_CONN_STREAM) != TCL_OK) {
		Ns_Log(Error, "make channel: error writing headers");
	    }
        }
    }

    if (Ns_SockSetBlocking(connPtr->sockPtr->sock) != TCL_OK) {
	Ns_Log(Error, "make channel: error while making channel blocking");
    }

    connPtr->sockPtr->sock = NS_INVALID_SOCKET;

    return chan;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
