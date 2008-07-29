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

NS_RCSID("@(#) $Header$");

static int GetChan(Tcl_Interp *interp, char *id, Tcl_Channel *chanPtr);
static int GetIndices(Tcl_Interp *interp, Conn *connPtr, Tcl_Obj **objv,
                      int *offPtr, int *lenPtr);
static Tcl_Channel MakeConnChannel(NsInterp *itPtr, Ns_Conn *conn);

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
Ns_ConnAuth(Ns_Conn *conn)
{
    return conn->auth;
}


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
Ns_ConnHeaders(Ns_Conn *conn)
{
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
Ns_ConnOutputHeaders(Ns_Conn *conn)
{
    return conn->outputheaders;
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

char *
Ns_ConnAuthUser(Ns_Conn *conn)
{
    return conn->auth ? Ns_SetIGet(conn->auth, "Username") : NULL;
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

char *
Ns_ConnAuthPasswd(Ns_Conn *conn)
{
    return conn->auth ? Ns_SetIGet(conn->auth, "Password") : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentLength --
 *
 *      Get the content length from the client
 *
 * Results:
 *      An integer content length, or 0 if none sent
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnContentLength(Ns_Conn *conn)
{
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

char *
Ns_ConnContent(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->reqPtr->content;
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

char *
Ns_ConnContentFile(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->sockPtr != NULL ? connPtr->sockPtr->tfile : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnContentFd --
 *
 *      Return opene file descriptor of the file with spooled content.
 *
 * Results:
 *      File descriptor or 0 if not used
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int Ns_ConnContentFd(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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

char *
Ns_ConnServer(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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
Ns_ConnResponseStatus(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->responseStatus;

}

void
Ns_ConnSetResponseStatus(Ns_Conn *conn, int new_status)
{
    Conn *connPtr = (Conn *) conn;

    if (new_status != 200) {
        connPtr->responseStatus = new_status;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnResponseVersion --
 *
 *      Get the reponse protocol and version that will be sent
 *
 * Results:
 *      String like HTTP/1.0
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConnResponseVersion(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->responseVersion;

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetResponseVersion --
 *
 *      Set the response protocol and version that will be sent
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
Ns_ConnSetResponseVersion(Ns_Conn *conn, char *new_version)
{
    Conn *connPtr = (Conn *) conn;

    if (connPtr->responseVersion != NULL) {
        ns_free(connPtr->responseVersion);
    }
    connPtr->responseVersion = ns_strcopy(new_version);
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

Tcl_WideInt
Ns_ConnContentSent(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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
Ns_ConnSetContentSent(Ns_Conn *conn, Tcl_WideInt length)
{
    Conn *connPtr = (Conn *) conn;

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

int
Ns_ConnResponseLength(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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

char *
Ns_ConnPeer(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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
Ns_ConnSetPeer(Ns_Conn *conn, struct sockaddr_in *saPtr)
{
    Conn *connPtr = (Conn *) conn;

    connPtr->reqPtr->port = ntohs(saPtr->sin_port);
    strcpy(connPtr->reqPtr->peer, ns_inet_ntoa(saPtr->sin_addr));
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
Ns_ConnPeerPort(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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
 *      Depreciated: Use Ns_SetConnLocationProc() which is virtual host
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
Ns_SetLocationProc(char *server, Ns_LocationProc *proc)
{
    NsServer *servPtr = NsGetServer(server);

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
 *      Depreciated: Use Ns_ConnLocationAppend() which is virtual host
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

char *
Ns_ConnLocation(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;
    NsServer *servPtr = connPtr->servPtr;
    char *location = NULL;

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
    NsServer   *servPtr = connPtr->servPtr;
    Ns_Set     *headers;
    char       *location, *host;

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

    } else if (servPtr->vhost.enabled
               && (headers = Ns_ConnHeaders(conn))
               && (host = Ns_SetIGet(headers, "Host"))
               && *host != '\0') {

        /*
         * Construct a location string from the HTTP host header.
         */

        if (!Ns_StrIsHost(host)) {
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

char *
Ns_ConnHost(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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
Ns_ConnPort(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

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

int
Ns_ConnSock(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return (connPtr->sockPtr ? connPtr->sockPtr->sock : -1);
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
Ns_ConnSockPtr(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return (Ns_Sock*)connPtr->sockPtr;
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
Ns_ConnSockContent(Ns_Conn *conn)
{
    Conn     *connPtr = (Conn*)conn;

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

char *
Ns_ConnDriverName(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->drvPtr->name;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnDriverContext --
 *
 *      Get the conn-wide context for this driver
 *
 * Results:
 *      The driver-supplied context
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void *
Ns_ConnDriverContext(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return (void *)(connPtr->sockPtr ? connPtr->sockPtr->arg : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnStartTime --
 *
 *      Return the connection start time, which is the time the
 *      connection was queued from the driver thread, not the time
 *      the underlying socket was opened to the server.
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

    return &connPtr->startTime;
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

int
Ns_ConnId(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->id;
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

int
Ns_ConnModifiedSince(Ns_Conn *conn, time_t since)
{
    Conn *connPtr = (Conn *) conn;

    if (connPtr->servPtr->opts.modsince) {
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
Ns_ConnGetEncoding(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->outputEncoding;
}

void
Ns_ConnSetEncoding(Ns_Conn *conn, Tcl_Encoding encoding)
{
    Conn *connPtr = (Conn *) conn;

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
Ns_ConnGetUrlEncoding(Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;

    return connPtr->urlEncoding;
}

void
Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding)
{
    Conn *connPtr = (Conn *) conn;

    connPtr->urlEncoding = encoding;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnNsvGet
 *
 *      Returns newly allocated variable value or NULL if not found or array does not exists
 *
 * Results:
 *      Allocated string
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConnNsvGet(char *aname, char *key)
{
    Conn *connPtr = (Conn*)Ns_GetConn();

    if (connPtr != NULL) {
        return NsNsvGet(connPtr->servPtr, aname, key);
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnNsvExists
 *
 * Results:
 *      Returns 1 if key exists in the given array, otherwise 0
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnNsvExists(char *aname, char *key)
{
    Conn *connPtr = (Conn*)Ns_GetConn();

    if (connPtr != NULL) {
        return NsNsvExists(connPtr->servPtr, aname, key);
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnNsvSet
 *
 *      Assign new value to the key in the given array
 *
 * Results:
 *      Returns NS_OK if set, NS_ERROR if array does not exists
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnNsvSet(char *aname, char *key, char *value)
{
    Conn *connPtr = (Conn*)Ns_GetConn();

    if (connPtr != NULL) {
        return NsNsvSet(connPtr->servPtr, aname, key, value);
    }
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnNsvIncr
 *
 *      Increases value of the counter in the given array, if
 *      key does not exists, it is created and set to 1
 *
 * Results:
 *      Returns new value of the counter
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnNsvIncr(char *aname, char *key, int count)
{
    Conn *connPtr = (Conn*)Ns_GetConn();

    if (connPtr != NULL) {
        return NsNsvIncr(connPtr->servPtr, aname, key, count);
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnNsvAppend
 *
 *      Append list of strings to the key, creates new if does not exists,
 *      last value must be NULL
 *
 * Results:
 *      Returns NS_OK if assigned, NS_ERROR if array is not found
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnNsvAppend(char *aname, char *key, ...)
{
    Conn *connPtr = (Conn*)Ns_GetConn();
    int rc = NS_ERROR;
    va_list ap;

    if (connPtr != NULL) {
        va_start(ap, key);
        rc = NsNsvAppendVA(connPtr->servPtr, aname, key, ap);
        va_end(ap);
    }
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnNsvUnset
 *
 *      Resets given key inthe array, if key is NULL, flushes the whole array
 *
 * Results:
 *      Returns NS_OK if flushed, NS_ERROR if array not found
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnNsvUnset(char *aname, char *key)
{
    Conn *connPtr = (Conn*)Ns_GetConn();

    if (connPtr != NULL) {
        return NsNsvUnset(connPtr->servPtr, aname, key);
    }
    return NS_ERROR;

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
NsTclConnObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsInterp *itPtr = arg;
    Ns_Conn *conn = itPtr->conn;
    Conn *connPtr = (Conn *) conn;
    Ns_Set *form;
    Ns_Request *request;
    Tcl_Encoding encoding;
    Tcl_Channel chan;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    FormFile *filePtr;
    Ns_DString ds;
    int idx, off, len, opt;

    static CONST char *opts[] = {
        "authpassword", "authuser", "auth",
        "close", "content", "contentlength", "contentsentlength",
        "contentfile",
        "copy", "channel", "driver", "encoding", "files", "fileoffset",
        "filelength", "fileheaders", "flags", "form", "headers",
        "host", "id", "isconnected", "location", "method",
        "outputheaders", "peeraddr", "peerport", "port", "protocol",
        "query", "request", "server", "sock", "start", "status", "timeout",
        "url", "urlc", "urlencoding", "urlv", "version",
        "responseversion", "versionstring", "keepalive",
        NULL
    };
    enum ISubCmdIdx {
        CAuthPasswordIdx, CAuthUserIdx, CAuthIdx,
        CCloseIdx, CContentIdx, CContentLengthIdx, CContentSentLenIdx,
        CContentFileIdx,
        CCopyIdx, CChannelIdx, CDriverIdx, CEncodingIdx,
        CFilesIdx, CFileOffIdx, CFileLenIdx, CFileHdrIdx, CFlagsIdx,
        CFormIdx, CHeadersIdx, CHostIdx, CIdIdx, CIsConnectedIdx,
        CLocationIdx, CMethodIdx, COutputHeadersIdx, CPeerAddrIdx,
        CPeerPortIdx, CPortIdx, CProtocolIdx, CQueryIdx, CRequestIdx,
        CServerIdx, CSockIdx, CStartIdx, CStatusIdx, CTimeoutIdx, CUrlIdx,
        CUrlcIdx, CUrlEncodingIdx, CUrlvIdx, CVersionIdx,
        CResponseVersionIdx, CVersionStringIdx, CKeepAliveIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    /*
     * Only the "isconnected" option operates without a conn.
     */

    if (opt == CIsConnectedIdx) {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(connPtr ? 1 : 0));
        return TCL_OK;
    }
    if (connPtr == NULL) {
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
        if (itPtr->nsconn.flags & CONN_TCLAUTH) {
            Tcl_SetResult(interp, itPtr->nsconn.auth, TCL_STATIC);
        } else {
            if (connPtr->auth == NULL) {
                connPtr->auth = Ns_SetCreate(NULL);
            }
            Ns_TclEnterSet(interp, connPtr->auth, NS_TCL_SET_STATIC);
            strcpy(itPtr->nsconn.auth, Tcl_GetStringResult(interp));
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
        if (objc != 2 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "?off len?");
            return TCL_ERROR;
        }
        if (objc == 2) {
            Tcl_SetResult(interp, Ns_ConnContent(conn), TCL_STATIC);
        } else {
            if (GetIndices(interp, connPtr, objv+2, &off, &len) != TCL_OK) {
                return TCL_ERROR;
            }
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj(Ns_ConnContent(conn)+off, len));
        }
        break;

    case CContentLengthIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(conn->contentLength));
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
            CONST char *charset = Ns_GetEncodingCharset(connPtr->outputEncoding);
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
                && (itPtr->nsconn.flags & CONN_TCLFORM)) {

                Ns_ConnClearQuery(conn);
                itPtr->nsconn.flags ^= CONN_TCLFORM;
            }
            connPtr->urlEncoding = encoding;
        }
        if (connPtr->urlEncoding != NULL) {
            CONST char *charset = Ns_GetEncodingCharset(connPtr->urlEncoding);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(charset, -1));
        }
        break;

    case CPeerAddrIdx:
        Tcl_SetResult(interp, Ns_ConnPeer(conn), TCL_STATIC);
        break;

    case CPeerPortIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_ConnPeerPort(conn)));
        break;

    case CHeadersIdx:
        if (itPtr->nsconn.flags & CONN_TCLHDRS) {
            Tcl_SetResult(interp, itPtr->nsconn.hdrs, TCL_STATIC);
        } else {
            Ns_TclEnterSet(interp, connPtr->headers, NS_TCL_SET_STATIC);
            strcpy(itPtr->nsconn.hdrs, Tcl_GetStringResult(interp));
            itPtr->nsconn.flags |= CONN_TCLHDRS;
        }
        break;

    case COutputHeadersIdx:
        if (itPtr->nsconn.flags & CONN_TCLOUTHDRS) {
            Tcl_SetResult(interp, itPtr->nsconn.outhdrs, TCL_STATIC);
        } else {
            Ns_TclEnterSet(interp, connPtr->outputheaders, NS_TCL_SET_STATIC);
            strcpy(itPtr->nsconn.outhdrs, Tcl_GetStringResult(interp));
            itPtr->nsconn.flags |= CONN_TCLOUTHDRS;
        }
        break;

    case CFormIdx:
        if (itPtr->nsconn.flags & CONN_TCLFORM) {
            Tcl_SetResult(interp, itPtr->nsconn.form, TCL_STATIC);
        } else {
            form = Ns_ConnGetQuery(conn);
            if (form == NULL) {
                itPtr->nsconn.form[0] = '\0';
            } else {
                Ns_TclEnterSet(interp, form, NS_TCL_SET_STATIC);
                strcpy(itPtr->nsconn.form, Tcl_GetStringResult(interp));
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

    case CFileOffIdx: /* FALL-THRU */
    case CFileLenIdx: /* FALL-THRU */
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
        if (opt == CFileOffIdx) {
            Tcl_SetObjResult(interp, Tcl_NewLongObj((long) filePtr->off));
        } else if (opt == CFileLenIdx) {
            Tcl_SetObjResult(interp, Tcl_NewLongObj((long) filePtr->len));
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

    case CResponseVersionIdx:
        if (objc > 2) {
            Ns_ConnSetResponseVersion(conn, Tcl_GetString(objv[2]));
        }
        Tcl_SetResult(interp, Ns_ConnResponseVersion(conn), TCL_STATIC);
        break;

    case CRequestIdx:
        Tcl_SetResult(interp, request->line, TCL_STATIC);
        break;

    case CMethodIdx:
        Tcl_SetResult(interp, request->method, TCL_STATIC);
        break;

    case CProtocolIdx:
        Tcl_SetResult(interp, request->protocol, TCL_STATIC);
        break;

    case CHostIdx:
        Tcl_SetResult(interp, request->host, TCL_STATIC);
        break;

    case CPortIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(request->port));
        break;

    case CUrlIdx:
        Tcl_SetResult(interp, request->url, TCL_STATIC);
        break;

    case CQueryIdx:
        Tcl_SetResult(interp, request->query, TCL_STATIC);
        break;

    case CUrlcIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(request->urlc));
        break;

    case CVersionIdx:
        Tcl_SetObjResult(interp, Tcl_NewDoubleObj(request->version));
        break;

    case CVersionStringIdx:
        Tcl_SetResult(interp, request->versionstring, TCL_STATIC);
        break;

    case CLocationIdx:
        Ns_DStringInit(&ds);
        Ns_ConnLocationAppend(conn, &ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case CDriverIdx:
        Tcl_SetResult(interp, Ns_ConnDriverName(conn), TCL_STATIC);
        break;

    case CServerIdx:
        Tcl_SetResult(interp, Ns_ConnServer(conn), TCL_STATIC);
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
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_ConnSock(conn)));
        break;

    case CIdIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_ConnId(conn)));
        break;

    case CFlagsIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(connPtr->flags));
        break;

    case CStartIdx:
        Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&connPtr->startTime));
        break;

    case CCloseIdx:
        if (Ns_ConnClose(conn) != NS_OK) {
            Tcl_SetResult(interp, "could not close connection", TCL_STATIC);
            return TCL_ERROR;
        }
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
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj(connPtr->nContentSent));
        } else if (objc == 3) {
            if (Tcl_GetWideIntFromObj(interp, objv[2], &connPtr->nContentSent)
                != TCL_OK) {
                return TCL_ERROR;
            }
        } else {
            Tcl_WrongNumArgs(interp, 2, objv, "?value?");
            return TCL_ERROR;
        }
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
NsTclLocationProcObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                        Tcl_Obj *CONST objv[])
{
    NsServer *servPtr = NsGetInitServer();
    Ns_TclCallback *cbPtr;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        return TCL_ERROR;
    }
    if (servPtr == NULL) {
        Tcl_AppendResult(interp, "no initializing server", TCL_STATIC);
        return TCL_ERROR;
    }
    cbPtr = Ns_TclNewCallback(interp, NsTclConnLocation, objv[1],
                              objc - 2, objv + 2);
    Ns_SetConnLocationProc(NsTclConnLocation, cbPtr);

    return TCL_OK;
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
NsTclWriteContentObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                        Tcl_Obj **objv)
{
    NsInterp *itPtr = arg;
    int toCopy = 0;
    char *chanName;
    Request *reqPtr;
    Tcl_Channel chan;

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
    Tcl_Flush(chan);
    reqPtr = ((Conn *)itPtr->conn)->reqPtr;
    if (toCopy > reqPtr->avail || toCopy <= 0) {
        toCopy = reqPtr->avail;
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

    if (Ns_TclEvalCallback(interp, cbPtr, dest, NULL) != NS_OK) {
        Ns_TclLogError(interp);
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
GetChan(Tcl_Interp *interp, char *id, Tcl_Channel *chanPtr)
{
    Tcl_Channel chan;
    int mode;

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
GetIndices(Tcl_Interp *interp, Conn *connPtr, Tcl_Obj **objv, int *offPtr,
           int *lenPtr)
{
    int off, len;

    if (Tcl_GetIntFromObj(interp, objv[0], &off) != TCL_OK
        ||
        Tcl_GetIntFromObj(interp, objv[1], &len) != TCL_OK) {
        return TCL_ERROR;
    }
    if (off < 0 || off > connPtr->reqPtr->length) {
        Tcl_AppendResult(interp, "invalid offset: ", Tcl_GetString(objv[0]),
                         NULL);
        return TCL_ERROR;
    }
    if (len < 0 || len > (connPtr->reqPtr->length - off)) {
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
MakeConnChannel(NsInterp *itPtr, Ns_Conn *conn)
{
    Conn       *connPtr = (Conn *) conn;
    Tcl_Channel chan;

    if ((connPtr->flags & NS_CONN_CLOSED)) {
        Tcl_AppendResult(itPtr->interp, "connection closed", NULL);
        return NULL;
    }

    if (connPtr->sockPtr->sock == INVALID_SOCKET) {
        Tcl_AppendResult(itPtr->interp, "no socket for connection", NULL);
        return NULL;
    }

    /*
     * Create Tcl channel arround the connection socket
     */

    chan = Tcl_MakeTcpClientChannel((ClientData)connPtr->sockPtr->sock);
    if (chan == NULL) {
        Tcl_AppendResult(itPtr->interp, Tcl_PosixError(itPtr->interp), NULL);
        return NULL;
    }

    /*
     * Disable keep-alive and chunking headers.
     */

    if (connPtr->responseLength < 0) {
        connPtr->keep = 0;
    }

    /*
     * Check to see if HTTP headers are required and flush
     * them now before the conn socket is dissociated.
     */

    if (!(conn->flags & NS_CONN_SENTHDRS)) {
        if (!(itPtr->nsconn.flags & CONN_TCLHTTP)) {
            conn->flags |= NS_CONN_SKIPHDRS;
        } else {
            Ns_ConnWriteData(conn, NULL, 0, 0);
        }
    }

    Ns_SockSetBlocking(connPtr->sockPtr->sock);
    connPtr->sockPtr->sock = INVALID_SOCKET;

    return chan;
}
