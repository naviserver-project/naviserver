/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1(the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis,WITHOUT WARRANTY OF ANY KIND,either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright(C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively,the contents of this file may be used under the terms
 * of the GNU General Public License(the "GPL"),in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License,indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above,a recipient may use your
 * version of this file under either the License or the GPL.
 *
 */

static const char *RCSID = "@(#) $Header$, compiled: " __DATE__ " " __TIME__;

/* 
 * nslog.c --
 *
 *	This file implements the access log using NCSA Common Log format.
 *
 *
 *  The nslog module implements Common Log Format access logging.  This
 *  format can be used by any web analyzer tool.  It optionally supports
 *  NCSA Combined Log Format and supports log file rolling.  The log files
 *  are stored in the server/server1/modules/nslog directory or can be
 *  specified in the config file.
 *
 * Known Issues
 *
 *  When supressquery is true, the side-effect is that the real URI is
 *  returned,so places where trailing slash returns "index.html" logs as
 *  "index.html".
 *
 * Sample Configuration
 *
 * ns_section "ns/server/${servername}/module/nslog"
 * ns_param   file            access.log                 ;# path to the log file
 * ns_param   formattedtime   true                       ;# true=common log format
 * ns_param   logcombined     false                      ;# true==NCSA combined format
 * ns_param   maxbuffer       0                          ;# Max number of lines to keep in the buffer before flushing to disk
 * ns_param   maxbackup       5                          ;# Max number to keep around when rolling
 * ns_param   rollhour        0                          ;# Time to roll log
 * ns_param   rolllog         true                       ;# Should we roll log?
 * ns_param   rollonsignal    true                       ;# Roll log on SIGHUP
 * ns_param   suppressquery   false                      ;# true==Do not show query string in the log
 * ns_param   checkforproxy   false                      ;# true==check for X-Forwarded-For header
 * ns_param   extendedheaders "Referer X-Forwarded-For"  ;# Tcl list of additional headers
 *
 * ns_accesslog Command
 *
 * Once loaded nslog modules introduces ns_accesslog command with the folowing options:
 *
 * roll - perform access.log rolling now
 * file - set/return access log file
 * rollfmt - set/return folling format
 * flags - set/return current flags such as logcombined/formattedtime/suppressquery/checkforproxy
 * maxbuffer - set/return max number of lines in the buffer
 * maxbackup - set/return max number of backup files
 * extendedheaders - set/return extended headers to be logged
 *
 *  Every command except roll without parameter just returns current
 *  value, if given third arguments new value will be set.
 *
 *  Example:
 *
 * nscp:1 > ns_accesslog file
 * /usr/local/aolserver/logs/access.log
 *
 * nscp:2 > ns_accesslog flags
 * logCombined formattedTime
 *
 * nscp:3 > ns_accesslog flags { logCombined formattedTime checkForProxy }
 * logCombined formattedTime checkForProxy
 *
 * nscp:1 > ns_accesslog extendedheaders
 *
 * nscp:1 > ns_accesslog extendedheaders { X-Forwarded-For Accepted }
 * X-Forwarded-For Accepted
 *
 */

#include "ns.h"
#include <sys/stat.h>	/* mkdir */
#include <ctype.h>	/* isspace */

#define LOG_COMBINED	        1
#define LOG_FMTTIME	        2
#define LOG_REQTIME	        4
#define LOG_CHECKFORPROXY	8
#define LOG_SUPPRESSQUERY	16

int Ns_ModuleVersion = 1;

typedef struct {
    Ns_Mutex	   lock;
    char	   *module;
    char           *file;
    char           *rollfmt;
    char           **extheaders;
    int            fd;
    int		   flags;
    int            maxbackup;
    int            maxlines;
    int            curlines;
    Ns_DString     buffer;
} Log;


/*
 * Local functions defined in this file
 */
static Ns_Callback LogRollCallback;
static Ns_Callback LogCloseCallback;
static Ns_TraceProc LogTrace;
static int LogFlush(Log *logPtr,Ns_DString *dsPtr);
static int LogOpen(Log *logPtr);
static int LogRoll(Log *logPtr);
static int LogClose(Log *logPtr);
static Ns_ArgProc LogArg;
static Tcl_CmdProc LogCmd;
static Ns_TclInterpInitProc AddCmds;

/*
 * Static variables defined in this file
 */


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *	Module initialization routine.
 *
 * Results:
 *	NS_OK.
 *
 * Side effects:
 *	Log file is opened,trace routine is registered,and,if
 *	configured,log file roll signal and scheduled procedures 
 *	are registered.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ModuleInit(char *server,char *module)
{
    char *path;
    Log	*logPtr;
    int opt,hour;
    Ns_DString ds;
    static int first = 1;

    /* Register the info callbacks just once. */
    if (first) {
        Ns_RegisterProcInfo((void *)LogRollCallback,"logroll",LogArg);
        Ns_RegisterProcInfo((void *)LogCloseCallback,"logclose",LogArg);
        first = 0;
    }

    Ns_DStringInit(&ds);
    /* Initialize the log buffer. */
    logPtr = ns_calloc(1,sizeof(Log));
    logPtr->fd = -1;
    logPtr->module = module;
    Ns_MutexInit(&logPtr->lock);
    Ns_MutexSetName2(&logPtr->lock,"nslog",server);
    Ns_DStringInit(&logPtr->buffer);

    /*
     * Determine the log file name which,if not
     * absolute,is expected to exist in the module
     * specific directory.  The module directory is
     * created if necessary.
     */

    path = Ns_ConfigGetPath(server,module,NULL);
    logPtr->file = Ns_ConfigGetValue(path,"file");
    if (logPtr->file == NULL) {
        logPtr->file = "access.log";
    }
    logPtr->file = ns_strdup(logPtr->file);

    if (Ns_PathIsAbsolute(logPtr->file) == NS_FALSE) {
        Ns_ModulePath(&ds,server,module,NULL,NULL);
        if (mkdir(ds.string,0755) != 0 && 
            errno != EEXIST && 
            errno != EISDIR) {
            Ns_Log(Error,"nslog: mkdir(%s) failed: %s",ds.string,strerror(errno));
            Ns_DStringFree(&ds);
            return NS_ERROR;
       }
       Ns_DStringTrunc(&ds,0);
       Ns_ModulePath(&ds,server,module,logPtr->file,NULL);
       logPtr->file = Ns_DStringExport(&ds);
    }

    /* Get parameters from configuration file */
    logPtr->rollfmt = Ns_ConfigGetValue(path,"rollfmt");
    if (logPtr->rollfmt == NULL) {
        logPtr->rollfmt = ns_strdup(logPtr->rollfmt);
    }
    if (!Ns_ConfigGetInt(path,"maxbackup",&logPtr->maxbackup) || 
        logPtr->maxbackup < 1) {
        logPtr->maxbackup = 100;
    }
    if (!Ns_ConfigGetInt(path,"maxbuffer",&logPtr->maxlines))  {
        logPtr->maxlines = 0;
    }
    if (!Ns_ConfigGetBool(path,"formattedTime",&opt) || opt)  {
        logPtr->flags |= LOG_FMTTIME;
    }
    if (!Ns_ConfigGetBool(path,"logcombined",&opt) || opt) {
        logPtr->flags |= LOG_COMBINED;
    }
    if (Ns_ConfigGetBool(path,"logreqtime",&opt) && opt) {
        logPtr->flags |= LOG_REQTIME;
    }
    if (Ns_ConfigGetBool(path,"suppressquery",&opt) && opt) {
        logPtr->flags |= LOG_SUPPRESSQUERY;
    }
    if (Ns_ConfigGetBool(path,"checkforproxy",&opt) && opt) {
        logPtr->flags |= LOG_CHECKFORPROXY;
    }
    /* Schedule various log roll and shutdown options. */
    if (!Ns_ConfigGetInt(path,"rollhour",&hour) || hour < 0 || hour > 23) {
        hour = 0;
    }
    if (!Ns_ConfigGetBool(path,"rolllog",&opt) || opt) {
        Ns_ScheduleDaily((Ns_SchedProc *) LogRollCallback,logPtr,0,hour,0,NULL);
    }
    if (Ns_ConfigGetBool(path,"rollonsignal",&opt) && opt) {
        Ns_RegisterAtSignal(LogRollCallback,logPtr);
    }
    /* Parse extended headers, it is just Tcl list of names */
    Ns_DStringTrunc(&ds,0);
    Ns_DStringVarAppend(&ds,Ns_ConfigGet(path,"extendedheaders"),0);
    if (Tcl_SplitList(NULL,ds.string,&opt,&logPtr->extheaders) != TCL_OK) {
        Ns_Log(Error,"nslog: invalid %s/extendedHeaders parameter: %s",path,ds.string);
    }
    Ns_DStringFree(&ds);

    /* Open the log and register the trace. */
    if (LogOpen(logPtr) != NS_OK) {
        return NS_ERROR;
    }
    Ns_RegisterServerTrace(server,LogTrace,logPtr);
    Ns_RegisterAtShutdown(LogCloseCallback,logPtr);
    Ns_TclInitInterps(server,AddCmds,logPtr);
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogTrace --
 *
 *	Trace routine for appending the log with the current connection
 *	results.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Entry is appended to the open log.
 *
 *----------------------------------------------------------------------
 */

static void 
LogTrace(void *arg,Ns_Conn *conn)
{
    Log	*logPtr = arg;
    char buf[100];
    Ns_DString ds;
    Ns_Time now,diff;
    int quote,n,status;
    register char **h,*p = 0;

    /* Compute the request's elapsed time. */
    if (logPtr->flags & LOG_REQTIME) {
        Ns_GetTime(&now);
        Ns_DiffTime(&now,Ns_ConnStartTime(conn),&diff);
    }

    Ns_DStringInit(&ds);

    /*
     * Append the peer address and auth user(if any).
     * Watch for users comming from proxy servers if configured.
     */

    if (logPtr->flags & LOG_CHECKFORPROXY) {
        p = Ns_SetIGet(conn->headers,"X-Forwarded-For");
        if (p != NULL && !strcasecmp(p,"unknown")) {
            p = 0;
        }
    }
    Ns_DStringAppend(&ds,p && *p ? p : Ns_ConnPeer(conn));

    if (conn->authUser == NULL) {
        Ns_DStringAppend(&ds," - - ");
    } else {
        p = conn->authUser;
        quote = 0;
        while (*p != '\0') {
	  if (isspace((unsigned char) *p)) {
	      quote = 1;
	      break;
	   }
	 ++p;
       }
       if (quote) {
           Ns_DStringVarAppend(&ds," - \"",conn->authUser,"\" ",NULL);
       } else {
           Ns_DStringVarAppend(&ds," - ",conn->authUser," ",NULL);
       }
    }

    /* Append a common log format time stamp including GMT offset. */
    if (!(logPtr->flags & LOG_FMTTIME)) {
        sprintf(buf,"[%ld]",(long)time(NULL));
    } else {
       Ns_LogTime(buf);
    }
    Ns_DStringAppend(&ds,buf);

    /* Append the request line. */
    if (conn->request && conn->request->line) {
        if (logPtr->flags & LOG_SUPPRESSQUERY) {
	    /* Don't display query data. */
	    Ns_DStringVarAppend(&ds," \"",conn->request->url,"\" ",NULL);
        } else {
            Ns_DStringVarAppend(&ds," \"",conn->request->line,"\" ",NULL);
        }
    } else {
        Ns_DStringAppend(&ds," \"\" ");
    }
    /* Construct and append the HTTP status code and bytes sent. */
    n = Ns_ConnResponseStatus(conn);
    sprintf(buf,"%d %u ",n ? n : 200,Ns_ConnContentSent(conn));
    Ns_DStringAppend(&ds,buf);

    if ((logPtr->flags & LOG_COMBINED)) {
        /* Append the referer and user-agent headers(if any). */
        Ns_DStringAppend(&ds,"\"");
        if ((p = Ns_SetIGet(conn->headers,"referer"))) {
            Ns_DStringAppend(&ds,p);
        }
        Ns_DStringAppend(&ds,"\" \"");
        if ((p = Ns_SetIGet(conn->headers,"user-agent"))) {
            Ns_DStringAppend(&ds,p);
        }
        Ns_DStringAppend(&ds,"\"");
    }

    /* Append the request's elapsed time(if enabled). */
    if (logPtr->flags & LOG_REQTIME) {
        sprintf(buf," %d.%06ld",(int)diff.sec,diff.usec);
        Ns_DStringAppend(&ds,buf);
    }

    /* Append the extended headers(if any). */
    for (h = logPtr->extheaders; *h; h++) {
        if (!(p = Ns_SetIGet(conn->headers,*h))) {
            p = "";
        }
        Ns_DStringVarAppend(&ds," \"",p,"\"",NULL);
    }

    /* Append the trailing newline and buffer and/or flush the line. */
    status = NS_OK;
    Ns_DStringAppend(&ds,"\n");
    Ns_MutexLock(&logPtr->lock);
    if (logPtr->maxlines <= 0) {
        status = LogFlush(logPtr,&ds);
    } else {
        Ns_DStringNAppend(&logPtr->buffer,ds.string,ds.length);
        if (++logPtr->curlines > logPtr->maxlines) {
	    status = LogFlush(logPtr,&logPtr->buffer);
	    logPtr->curlines = 0;
        }
    }
    Ns_MutexUnlock(&logPtr->lock);
    Ns_DStringFree(&ds);
    if (status != NS_OK) {
        Ns_Log(Error,"nslog: failed to flush log: %s",strerror(errno));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * LogCmd --
 *
 *	Implement the ns_accesslog command.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Depends on command.
 *
 *----------------------------------------------------------------------
 */

static int
AddCmds(Tcl_Interp *interp,void *arg)
{
    Tcl_CreateCommand(interp,"ns_accesslog",LogCmd,arg,NULL);
    return TCL_OK;
}

static int
LogCmd(ClientData arg,Tcl_Interp *interp,int argc,CONST char **argv)
{
    int status;
    Ns_DString ds;
    Log *logPtr = arg;

    if (argc < 2) {
        Tcl_AppendResult(interp,"wrong # args: should be: \"",argv[0]," command ?arg?\"",NULL);
        return TCL_ERROR;
    }

    if (STREQ(argv[1],"rollfmt")) {
        Ns_MutexLock(&logPtr->lock);
        if(argc > 2) {
           ns_free(logPtr->rollfmt);
           logPtr->rollfmt = ns_strdup(argv[2]);
        }
        Ns_MutexUnlock(&logPtr->lock);
        Tcl_SetResult(interp,logPtr->rollfmt,TCL_STATIC);
    } else

    if (STREQ(argv[1],"maxbackup")) {
        Ns_MutexLock(&logPtr->lock);
        if(argc > 2 && (logPtr->maxbackup = atoi(argv[2])) < 1) {
           logPtr->maxbackup = 100;
        }
        Tcl_SetObjResult(interp,Tcl_NewIntObj(logPtr->maxbackup));
        Ns_MutexUnlock(&logPtr->lock);
    } else

    if (STREQ(argv[1],"maxbuffer")) {
        Ns_MutexLock(&logPtr->lock);
        if (argc > 2) {
            logPtr->maxlines = atoi(argv[2]);
        }
        Tcl_SetObjResult(interp,Tcl_NewIntObj(logPtr->maxlines));
        Ns_MutexUnlock(&logPtr->lock);
    } else

    if (STREQ(argv[1],"extendedheaders")) {
        char **h,**h1,**h2 = logPtr->extheaders;
        Ns_DStringInit(&ds);
        if (argc > 2) {
            if(Tcl_SplitList(interp,argv[2],&status,&h1) != TCL_OK) {
               return TCL_ERROR;
            }
            Ns_MutexLock(&logPtr->lock);
            /* Wait for others to finish logging extheaders */
            sleep(1);
            logPtr->extheaders = h1;
            Ns_MutexUnlock(&logPtr->lock);
            if (h2) {
                Tcl_Free((char*)h2);
            }
       }
       for (h = logPtr->extheaders; *h; h++) {
           Ns_DStringVarAppend(&ds,*h," ",NULL);
       }
       Tcl_AppendResult(interp,ds.string,0);
       Ns_DStringFree(&ds);
    } else

    if (STREQ(argv[1],"flags")) {
        Ns_DStringInit(&ds);
        if (argc > 2) {
            status = 0;
            Ns_DStringAppend(&ds,argv[2]);
            Ns_StrToLower(ds.string);
            if (strstr(ds.string,"logcombined")) {
                status |= LOG_COMBINED;
            }
            if (strstr(ds.string,"formattedtime")) {
                status |= LOG_FMTTIME;
            }
            if (strstr(ds.string,"logreqtime")) {
                status |= LOG_REQTIME;
            }
            if (strstr(ds.string,"checkforproxy")) {
                status |= LOG_CHECKFORPROXY;
            }
            if (strstr(ds.string,"suppressquery")) {
                status |= LOG_SUPPRESSQUERY;
            } 
            logPtr->flags = status;
            Ns_DStringTrunc(&ds,0);
       }
       if (logPtr->flags & LOG_COMBINED) {
           Ns_DStringAppend(&ds,"logCombined ");
       }
       if (logPtr->flags & LOG_FMTTIME) {
           Ns_DStringAppend(&ds,"formattedTime ");
       }
       if (logPtr->flags & LOG_REQTIME) {
           Ns_DStringAppend(&ds,"logReqTime ");
       }
       if (logPtr->flags & LOG_CHECKFORPROXY) {
           Ns_DStringAppend(&ds,"checkForProxy ");
       }
       if (logPtr->flags & LOG_SUPPRESSQUERY) {
           Ns_DStringAppend(&ds,"suppressQuery ");
       }
       Tcl_AppendResult(interp,ds.string,0);
       Ns_DStringFree(&ds);
    } else

    if (STREQ(argv[1],"file")) {
        if (argc > 2 && Ns_PathIsAbsolute(argv[2])) {
            Ns_MutexLock(&logPtr->lock);
            LogClose(logPtr);
            logPtr->file = ns_strdup(argv[2]);
            LogOpen(logPtr);
            Ns_MutexUnlock(&logPtr->lock);
       }
       Tcl_SetResult(interp,logPtr->file,TCL_STATIC);
    } else

    if (STREQ(argv[1],"roll")) {
        if (argc != 2 && argc != 3) {
            Tcl_AppendResult(interp,"wrong # args: should be: \"",argv[0]," ",argv[1]," ?file?\"",NULL);
            return TCL_ERROR;
        }
        Ns_MutexLock(&logPtr->lock);
        if (argc == 2) {
	    status = LogRoll(logPtr);
      } else {
	    status = NS_OK;
    	    if (access(argv[2],F_OK) == 0) {
                status = Ns_RollFile(argv[2],logPtr->maxbackup);
            }
	    if (status == NS_OK) {
	        if (rename(logPtr->file,argv[2]) != 0) {
	            status = NS_ERROR;
	        } else {
	            LogFlush(logPtr,&logPtr->buffer);
	            status = LogOpen(logPtr);
	        }
	    }
      }
      Ns_MutexUnlock(&logPtr->lock);
      if (status != NS_OK) {
	  Tcl_AppendResult(interp,"could not roll \"",logPtr->file,"\": ",Tcl_PosixError(interp),NULL);
          return TCL_ERROR;
      }
    } else {
        Tcl_AppendResult(interp,"unknown command \"",argv[1],"\": should be file or roll",NULL);
        return TCL_ERROR;
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogArg --
 *
 *	Copy log file as argument for callback query.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
LogArg(Tcl_DString *dsPtr,void *arg)
{
    Log *logPtr = arg;

    Tcl_DStringAppendElement(dsPtr,logPtr->file);
}


/*
 *----------------------------------------------------------------------
 *
 * LogOpen --
 *
 *      Open the access log,closing previous log if opeopen
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Log re-opened.
 *
 *----------------------------------------------------------------------
 */

static int
LogOpen(Log *logPtr)
{
    int fd;

    fd = open(logPtr->file,O_APPEND|O_WRONLY|O_CREAT,0644);
    if (fd < 0) {
        Ns_Log(Error,"nslog: error '%s' opening '%s'",strerror(errno),logPtr->file);
        return NS_ERROR;
    }
    if (logPtr->fd >= 0) {
        close(logPtr->fd);
    }
    logPtr->fd = fd;
    Ns_Log(Notice,"nslog: opened '%s'",logPtr->file);
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogClose --
 *
 *      Flush and/or close the log.
 *
 * Results:
 *      NS_TRUE or NS_FALSE if log was closed.
 *
 * Side effects:
 *      Buffer entries,if any,are flushed.
 *
 *----------------------------------------------------------------------
 */

static int
LogClose(Log *logPtr)
{
    int status;

    status = NS_OK;
    if (logPtr->fd >= 0) {
        Ns_Log(Notice,"nslog: closing '%s'",logPtr->file);
        status = LogFlush(logPtr,&logPtr->buffer);
        close(logPtr->fd);
        logPtr->fd = -1;
        Ns_DStringFree(&logPtr->buffer);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LogFlush --
 *
 *	Flush a log buffer to the open log file.  Note:  The mutex
 *	is assumed held during call. 
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Will disable the log on error.
 *
 *----------------------------------------------------------------------
 */

static int
LogFlush(Log *logPtr,Ns_DString *dsPtr)
{
    if (dsPtr->length > 0) {
        if (logPtr->fd >= 0 && 
            write(logPtr->fd,dsPtr->string,(size_t)dsPtr->length) != dsPtr->length) {
	    Ns_Log(Error,"nslog: logging disabled: write() failed: '%s'",strerror(errno));
            close(logPtr->fd);
            logPtr->fd = -1;
          }
          Ns_DStringTrunc(dsPtr,0);
    }
    if (logPtr->fd < 0) {
        return NS_ERROR;
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogRoll --
 *
 *      Roll and re-open the access log.  This procedure is scheduled
 *      and/or registered at signal catching.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Files are rolled to new names.
 *
 *----------------------------------------------------------------------
 */

static int
LogRoll(Log *logPtr)
{
    Ns_DString ds;
    struct tm *ptm;
    char timeBuf[512];
    int status = NS_OK;
    time_t now = time(0);

    LogClose(logPtr);
    if (access(logPtr->file,F_OK) == 0) {
        if (logPtr->rollfmt == NULL) {
	    status = Ns_RollFile(logPtr->file,logPtr->maxbackup);
      } else {
	    ptm = ns_localtime(&now);
	    strftime(timeBuf,512,logPtr->rollfmt,ptm);
	    Ns_DStringInit(&ds);
	    Ns_DStringVarAppend(&ds,logPtr->file,".",timeBuf,NULL);
	    if (access(ds.string,F_OK) == 0) {
	        status = Ns_RollFile(ds.string,logPtr->maxbackup);
	    } else
               if (errno != ENOENT) {
	           Ns_Log(Error,"nslog: access(%s,F_OK) failed: '%s'",ds.string,strerror(errno));
	           status = NS_ERROR;
	       }
	       if (status == NS_OK && rename(logPtr->file,ds.string) != 0) {
	           Ns_Log(Error,"nslog: rename(%s,%s) failed: '%s'",logPtr->file,ds.string,strerror(errno));
                   status = NS_ERROR;
	       }
	       Ns_DStringFree(&ds);
	       if (status == NS_OK) {
                   status = Ns_PurgeFiles(logPtr->file,logPtr->maxbackup);
               }
      }
    }
    status = LogOpen(logPtr);
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LogCloseCallback,LogRollCallback -
 *
 *      Close or roll the log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	See LogClose and LogRoll.
 *
 *----------------------------------------------------------------------
 */

static void
LogCallback(int(proc)(Log *),void *arg,char *desc)
{
    int status;
    Log *logPtr = arg;

    Ns_MutexLock(&logPtr->lock);
    status =(*proc)(logPtr);
    Ns_MutexUnlock(&logPtr->lock);
    if (status != NS_OK) {
        Ns_Log(Error,"nslog: failed: %s '%s': '%s'",desc,logPtr->file,strerror(errno));
    }
}

static void
LogCloseCallback(void *arg)
{
    LogCallback(LogClose,arg,"close");
}

static void
LogRollCallback(void *arg)
{
    LogCallback(LogRoll,arg,"roll");
}
