/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
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
 * tclcmds.c --
 *
 *      Connect Tcl command names to the functions that implement them.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

/*
 * Tcl object and string commands.
 */

extern Tcl_ObjCmdProc
    NsTclAdpAbortObjCmd,
    NsTclAdpAppendObjCmd,
    NsTclAdpArgcObjCmd,
    NsTclAdpArgvObjCmd,
    NsTclAdpBindArgsObjCmd,
    NsTclAdpBreakObjCmd,
    NsTclAdpCompressObjCmd,
    NsTclAdpDirObjCmd,
    NsTclAdpDumpObjCmd,
    NsTclAdpEvalObjCmd,
    NsTclAdpExceptionObjCmd,
    NsTclAdpIncludeObjCmd,
    NsTclAdpMimeTypeObjCmd,
    NsTclAdpParseObjCmd,
    NsTclAdpPutsObjCmd,
    NsTclAdpReturnObjCmd,
    NsTclAdpSafeEvalObjCmd,
    NsTclAdpStreamObjCmd,
    NsTclAdpTellObjCmd,
    NsTclAdpTruncObjCmd,
    NsTclAfterObjCmd,
    NsTclAtCloseObjCmd,
    NsTclAtExitObjCmd,
    NsTclAtShutdownObjCmd,
    NsTclAtSignalObjCmd,
    NsTclAtStartupObjCmd,
    NsTclCancelObjCmd,
    NsTclChanObjCmd,
    NsTclCondObjCmd,
    NsTclConnObjCmd,
    NsTclConnSendFpObjCmd,
    NsTclCritSecObjCmd,
    NsTclCryptObjCmd,
    NsTclDeleteCookieObjCmd,
    NsTclDummyObjCmd,
    NsTclFTruncateObjCmd,
    NsTclGetAddrObjCmd,
    NsTclGetCookieObjCmd,
    NsTclGetHostObjCmd,
    NsTclGetUrlObjCmd,
    NsTclGifSizeObjCmd,
    NsTclGmTimeObjCmd,
    NsTclGuessTypeObjCmd,
    NsTclHTUUDecodeObjCmd,
    NsTclHTUUEncodeObjCmd,
    NsTclHashPathObjCmd,
    NsTclHeadersObjCmd,
    NsTclHttpObjCmd,
    NsTclHttpTimeObjCmd,
    NsTclICtlObjCmd,
    NsTclInfoObjCmd,
    NsTclJobObjCmd,
    NsTclJpegSizeObjCmd,
    NsTclKillObjCmd,
    NsTclLocalTimeObjCmd,
    NsTclLocationProcObjCmd,
    NsTclLogCtlObjCmd,
    NsTclLogObjCmd,
    NsTclLogRollObjCmd,
    NsTclModulePathObjCmd,
    NsTclMutexObjCmd,
    NsTclNsvAppendObjCmd,
    NsTclNsvArrayObjCmd,
    NsTclNsvExistsObjCmd,
    NsTclNsvGetObjCmd,
    NsTclNsvIncrObjCmd,
    NsTclNsvLappendObjCmd,
    NsTclNsvNamesObjCmd,
    NsTclNsvSetObjCmd,
    NsTclNsvUnsetObjCmd,
    NsTclPagePathObjCmd,
    NsTclParseArgsObjCmd,
    NsTclParseHttpTimeObjCmd,
    NsTclParseQueryObjCmd,
    NsTclPauseObjCmd,
    NsTclPurgeFilesObjCmd,
    NsTclRWLockObjCmd,
    NsTclRandObjCmd,
    NsTclRegisterAdpObjCmd,
    NsTclRegisterFilterObjCmd,
    NsTclRegisterProcObjCmd,
    NsTclRegisterTraceObjCmd,
    NsTclRequestAuthorizeObjCmd,
    NsTclRespondObjCmd,
    NsTclResumeObjCmd,
    NsTclReturnBadRequestObjCmd,
    NsTclReturnErrorObjCmd,
    NsTclReturnFileObjCmd,
    NsTclReturnForbiddenObjCmd,
    NsTclReturnFpObjCmd,
    NsTclReturnNotFoundObjCmd,
    NsTclReturnNoticeObjCmd,
    NsTclReturnObjCmd,
    NsTclReturnRedirectObjCmd,
    NsTclReturnUnauthorizedObjCmd,
    NsTclRollFileObjCmd,
    NsTclSHA1ObjCmd,
    NsTclSchedDailyObjCmd,
    NsTclSchedObjCmd,
    NsTclSchedWeeklyObjCmd,
    NsTclSelectObjCmd,
    NsTclSemaObjCmd,
    NsTclServerObjCmd,
    NsTclServerPathObjCmd,
    NsTclServerRootProcObjCmd,
    NsTclSetCookieObjCmd,
    NsTclSetObjCmd,
    NsTclShutdownObjCmd,
    NsTclSleepObjCmd,
    NsTclSockAcceptObjCmd,
    NsTclSockCallbackObjCmd,
    NsTclSockCheckObjCmd,
    NsTclSockListenCallbackObjCmd,
    NsTclSockListenObjCmd,
    NsTclSockNReadObjCmd,
    NsTclSockOpenObjCmd,
    NsTclSockSetBlockingObjCmd,
    NsTclSockSetNonBlockingObjCmd,
    NsTclSocketPairObjCmd,
    NsTclStartContentObjCmd,
    NsTclStrftimeObjCmd,
    NsTclSymlinkObjCmd,
    NsTclTimeObjCmd,
    NsTclTmpNamObjCmd,
    NsTclTruncateObjCmd,
    NsTclUnRegisterObjCmd,
    NsTclUnscheduleObjCmd,
    NsTclUrl2FileObjCmd,
    NsTclUrlDecodeObjCmd,
    NsTclUrlEncodeObjCmd,
    NsTclVarObjCmd,
    NsTclWriteContentObjCmd,
    NsTclWriteFpObjCmd,
    NsTclWriteObjCmd;

extern Tcl_CmdProc
    NsTclAdpDebugCmd,
    NsTclAdpRegisterAdpCmd,
    NsTclAdpRegisterProcCmd,
    NsTclAdpStatsCmd,
    NsTclCacheFlushCmd,
    NsTclCacheKeysCmd,
    NsTclCacheNamesCmd,
    NsTclCacheSizeCmd,
    NsTclCacheStatsCmd,
    NsTclCharsetsCmd,
    NsTclConfigCmd,
    NsTclConfigSectionCmd,
    NsTclConfigSectionsCmd,
    NsTclEncodingForCharsetCmd,
    NsTclEnvCmd,
    NsTclHrefsCmd,
    NsTclLibraryCmd,
    NsTclMkTempCmd,
    NsTclParseHeaderCmd,
    NsTclQuoteHtmlCmd,
    NsTclRegisterTagCmd,
    NsTclShareCmd,
    NsTclStripHtmlCmd,
    NsTclThreadCmd,
    TclX_KeyldelObjCmd,
    TclX_KeylgetObjCmd,
    TclX_KeylkeysObjCmd,
    TclX_KeylsetObjCmd;

/*
 * The following structure defines a command to be created
 * in new interps.
 */

typedef struct Cmd {
    char *name;
    Tcl_CmdProc *proc;
    Tcl_ObjCmdProc *objProc;
} Cmd;

/*
 * The following commands are generic, available in the config
 * and virtual server interps.
 */

static Cmd basicCmds[] = {
    {"env", NsTclEnvCmd, NULL},
    {"keyldel", TclX_KeyldelObjCmd, NULL},
    {"keylget", TclX_KeylgetObjCmd, NULL},
    {"keylkeys", TclX_KeylkeysObjCmd, NULL},
    {"keylset", TclX_KeylsetObjCmd, NULL},
    {"ns_addrbyhost", NULL, NsTclGetAddrObjCmd},
    {"ns_after", NULL, NsTclAfterObjCmd},
    {"ns_atexit", NULL, NsTclAtExitObjCmd},
    {"ns_atshutdown", NULL, NsTclAtShutdownObjCmd},
    {"ns_atsignal", NULL, NsTclAtSignalObjCmd},
    {"ns_atstartup", NULL, NsTclAtStartupObjCmd},
    {"ns_base64decode", NULL, NsTclHTUUDecodeObjCmd},
    {"ns_base64encode", NULL, NsTclHTUUEncodeObjCmd},
    {"ns_cache_flush", NsTclCacheFlushCmd, NULL},
    {"ns_cache_keys", NsTclCacheKeysCmd, NULL},
    {"ns_cache_names", NsTclCacheNamesCmd, NULL},
    {"ns_cache_size", NsTclCacheSizeCmd, NULL},
    {"ns_cache_stats", NsTclCacheStatsCmd, NULL},
    {"ns_cancel", NULL, NsTclCancelObjCmd},
    {"ns_charsets", NsTclCharsetsCmd, NULL},
    {"ns_cleanup", NULL, NsTclDummyObjCmd},
    {"ns_cond", NULL, NsTclCondObjCmd},
    {"ns_config", NsTclConfigCmd, NULL},
    {"ns_configsection", NsTclConfigSectionCmd, NULL},
    {"ns_configsections", NsTclConfigSectionsCmd, NULL},
    {"ns_critsec", NULL, NsTclCritSecObjCmd},
    {"ns_crypt", NULL, NsTclCryptObjCmd},
    {"ns_encodingforcharset", NsTclEncodingForCharsetCmd, NULL},
    {"ns_env", NsTclEnvCmd, NULL},
    {"ns_event", NULL, NsTclCondObjCmd},
    {"ns_ftruncate", NULL, NsTclFTruncateObjCmd},
    {"ns_fmttime", NULL, NsTclStrftimeObjCmd},
    {"ns_gifsize", NULL, NsTclGifSizeObjCmd},
    {"ns_gmtime", NULL, NsTclGmTimeObjCmd},
    {"ns_guesstype", NULL, NsTclGuessTypeObjCmd},
    {"ns_hashpath", NULL, NsTclHashPathObjCmd},
    {"ns_hostbyaddr", NULL, NsTclGetHostObjCmd},
    {"ns_hrefs", NsTclHrefsCmd, NULL},
    {"ns_http", NULL, NsTclHttpObjCmd},
    {"ns_httptime", NULL, NsTclHttpTimeObjCmd},
    {"ns_info", NULL, NsTclInfoObjCmd},
    {"ns_init", NULL, NsTclDummyObjCmd},
    {"ns_job", NULL, NsTclJobObjCmd},
    {"ns_jpegsize", NULL, NsTclJpegSizeObjCmd},
    {"ns_kill", NULL, NsTclKillObjCmd},
    {"ns_localtime", NULL, NsTclLocalTimeObjCmd},
    {"ns_locationproc", NULL, NsTclLocationProcObjCmd},
    {"ns_log", NULL, NsTclLogObjCmd},
    {"ns_logctl", NULL, NsTclLogCtlObjCmd},
    {"ns_logroll", NULL, NsTclLogRollObjCmd},
    {"ns_mktemp", NsTclMkTempCmd, NULL},
    {"ns_modulepath", NULL, NsTclModulePathObjCmd},
    {"ns_mutex", NULL, NsTclMutexObjCmd},
    {"ns_pagepath", NULL, NsTclPagePathObjCmd},
    {"ns_parseargs", NULL, NsTclParseArgsObjCmd},
    {"ns_parseheader", NsTclParseHeaderCmd, NULL},
    {"ns_parsehttptime", NULL, NsTclParseHttpTimeObjCmd},
    {"ns_parsequery", NULL, NsTclParseQueryObjCmd},
    {"ns_pause", NULL, NsTclPauseObjCmd},
    {"ns_purgefiles", NULL, NsTclPurgeFilesObjCmd},
    {"ns_quotehtml", NsTclQuoteHtmlCmd, NULL},
    {"ns_rand", NULL, NsTclRandObjCmd},
    {"ns_resume", NULL, NsTclResumeObjCmd},
    {"ns_rollfile", NULL, NsTclRollFileObjCmd},
    {"ns_rwlock", NULL, NsTclRWLockObjCmd},
    {"ns_schedule_daily", NULL, NsTclSchedDailyObjCmd},
    {"ns_schedule_proc", NULL, NsTclSchedObjCmd},
    {"ns_schedule_weekly", NULL, NsTclSchedWeeklyObjCmd},
    {"ns_sema", NULL, NsTclSemaObjCmd},
    {"ns_serverpath", NULL, NsTclServerPathObjCmd},
    {"ns_serverrootproc", NULL, NsTclServerRootProcObjCmd},
    {"ns_set", NULL, NsTclSetObjCmd},
    {"ns_sha1", NULL, NsTclSHA1ObjCmd},
    {"ns_sleep", NULL, NsTclSleepObjCmd},
    {"ns_sockaccept", NULL, NsTclSockAcceptObjCmd},
    {"ns_sockblocking", NULL, NsTclSockSetBlockingObjCmd},
    {"ns_sockcallback", NULL, NsTclSockCallbackObjCmd},
    {"ns_sockcheck", NULL, NsTclSockCheckObjCmd},
    {"ns_socketpair", NULL, NsTclSocketPairObjCmd},
    {"ns_socklistencallback", NULL, NsTclSockListenCallbackObjCmd},
    {"ns_socknonblocking", NULL, NsTclSockSetNonBlockingObjCmd},
    {"ns_socknread", NULL, NsTclSockNReadObjCmd},
    {"ns_sockopen", NULL, NsTclSockOpenObjCmd},
    {"ns_socklisten", NULL, NsTclSockListenObjCmd},
    {"ns_sockselect", NULL, NsTclSelectObjCmd},
    {"ns_striphtml", NsTclStripHtmlCmd, NULL},
    {"ns_symlink", NULL, NsTclSymlinkObjCmd},
    {"ns_thread", NsTclThreadCmd, NULL},
    {"ns_time", NULL, NsTclTimeObjCmd},
    {"ns_tmpnam", NULL, NsTclTmpNamObjCmd},
    {"ns_truncate", NULL, NsTclTruncateObjCmd},
    {"ns_unschedule_proc", NULL, NsTclUnscheduleObjCmd},
    {"ns_urldecode", NULL, NsTclUrlDecodeObjCmd},
    {"ns_urlencode", NULL, NsTclUrlEncodeObjCmd},
    {"ns_uudecode", NULL, NsTclHTUUDecodeObjCmd},
    {"ns_uuencode", NULL, NsTclHTUUEncodeObjCmd},
    {"ns_writefp", NULL, NsTclWriteFpObjCmd},

    /*
     * Add more basic Tcl commands here.
     */

    {NULL, NULL}
};

/*
 * The following commands require the NsServer context and
 * are available only in virtual server interps.
 */

static Cmd servCmds[] = {
    {"_ns_adp_include", NULL, NsTclAdpIncludeObjCmd},
    {"ns_adp_abort", NULL, NsTclAdpAbortObjCmd},
    {"ns_adp_append", NULL, NsTclAdpAppendObjCmd},
    {"ns_adp_argc", NULL, NsTclAdpArgcObjCmd},
    {"ns_adp_argv", NULL, NsTclAdpArgvObjCmd},
    {"ns_adp_bind_args", NULL, NsTclAdpBindArgsObjCmd},
    {"ns_adp_break", NULL, NsTclAdpBreakObjCmd},
    {"ns_adp_compress", NULL, NsTclAdpCompressObjCmd},
    {"ns_adp_debug", NsTclAdpDebugCmd, NULL},
    {"ns_adp_dir", NULL, NsTclAdpDirObjCmd},
    {"ns_adp_dump", NULL, NsTclAdpDumpObjCmd},
    {"ns_adp_eval", NULL, NsTclAdpEvalObjCmd},
    {"ns_adp_exception", NULL, NsTclAdpExceptionObjCmd},
    {"ns_adp_mime", NULL, NsTclAdpMimeTypeObjCmd},
    {"ns_adp_mimetype", NULL, NsTclAdpMimeTypeObjCmd},
    {"ns_adp_parse", NULL, NsTclAdpParseObjCmd},
    {"ns_adp_puts", NULL, NsTclAdpPutsObjCmd},
    {"ns_adp_registeradp", NsTclAdpRegisterAdpCmd, NULL},
    {"ns_adp_registerproc", NsTclAdpRegisterProcCmd, NULL},
    {"ns_adp_registertag", NsTclAdpRegisterAdpCmd, NULL},
    {"ns_adp_return", NULL, NsTclAdpReturnObjCmd},
    {"ns_adp_safeeval", NULL, NsTclAdpSafeEvalObjCmd},
    {"ns_adp_stats", NsTclAdpStatsCmd, NULL},
    {"ns_adp_stream", NULL, NsTclAdpStreamObjCmd},
    {"ns_adp_tell", NULL, NsTclAdpTellObjCmd},
    {"ns_adp_trunc", NULL, NsTclAdpTruncObjCmd},
    {"ns_atclose", NULL, NsTclAtCloseObjCmd},
    {"ns_chan", NULL, NsTclChanObjCmd},
    {"ns_checkurl", NULL, NsTclRequestAuthorizeObjCmd},
    {"ns_conn", NULL, NsTclConnObjCmd},
    {"ns_conncptofp", NULL, NsTclWriteContentObjCmd},
    {"ns_connsendfp", NULL, NsTclConnSendFpObjCmd},
    {"ns_deletecookie", NULL, NsTclDeleteCookieObjCmd},
    {"ns_getcookie", NULL, NsTclGetCookieObjCmd},
    {"ns_geturl", NULL, NsTclGetUrlObjCmd},
    {"ns_headers", NULL, NsTclHeadersObjCmd},
    {"ns_ictl", NULL, NsTclICtlObjCmd},
    {"ns_library", NsTclLibraryCmd, NULL},
    {"ns_puts", NULL, NsTclAdpPutsObjCmd},
    {"ns_register_adp", NULL, NsTclRegisterAdpObjCmd},
    {"ns_register_adptag", NsTclRegisterTagCmd, NULL},
    {"ns_register_filter", NULL, NsTclRegisterFilterObjCmd},
    {"ns_register_proc", NULL, NsTclRegisterProcObjCmd},
    {"ns_register_trace", NULL, NsTclRegisterTraceObjCmd},
    {"ns_requestauthorize", NULL, NsTclRequestAuthorizeObjCmd},
    {"ns_respond", NULL, NsTclRespondObjCmd},
    {"ns_return", NULL, NsTclReturnObjCmd},
    {"ns_returnadminnotice", NULL, NsTclReturnNoticeObjCmd},
    {"ns_returnbadrequest", NULL, NsTclReturnBadRequestObjCmd},
    {"ns_returnerror", NULL, NsTclReturnErrorObjCmd},
    {"ns_returnfile", NULL, NsTclReturnFileObjCmd},
    {"ns_returnforbidden", NULL, NsTclReturnForbiddenObjCmd},
    {"ns_returnfp", NULL, NsTclReturnFpObjCmd},
    {"ns_returnnotfound", NULL, NsTclReturnNotFoundObjCmd},
    {"ns_returnnotice", NULL, NsTclReturnNoticeObjCmd},
    {"ns_returnredirect", NULL, NsTclReturnRedirectObjCmd},
    {"ns_returnunauthorized", NULL, NsTclReturnUnauthorizedObjCmd},
    {"ns_server", NULL, NsTclServerObjCmd},
    {"ns_setcookie", NULL, NsTclSetCookieObjCmd},
    {"ns_share", NsTclShareCmd, NULL},
    {"ns_shutdown", NULL, NsTclShutdownObjCmd},
    {"ns_startcontent", NULL, NsTclStartContentObjCmd},
    {"ns_unregister_adp", NULL, NsTclUnRegisterObjCmd},
    {"ns_unregister_proc", NULL, NsTclUnRegisterObjCmd},
    {"ns_url2file", NULL, NsTclUrl2FileObjCmd},
    {"ns_var", NULL, NsTclVarObjCmd},
    {"ns_write", NULL, NsTclWriteObjCmd},
    {"ns_writecontent", NULL, NsTclWriteContentObjCmd},
    {"nsv_append", NULL, NsTclNsvAppendObjCmd},
    {"nsv_array", NULL, NsTclNsvArrayObjCmd},
    {"nsv_exists", NULL, NsTclNsvExistsObjCmd},
    {"nsv_get", NULL, NsTclNsvGetObjCmd},
    {"nsv_incr", NULL, NsTclNsvIncrObjCmd},
    {"nsv_lappend", NULL, NsTclNsvLappendObjCmd},
    {"nsv_names", NULL, NsTclNsvNamesObjCmd},
    {"nsv_set", NULL, NsTclNsvSetObjCmd},
    {"nsv_unset", NULL, NsTclNsvUnsetObjCmd},

    /*
     * Add more server Tcl commands here.
     */

    {NULL, NULL}
};


/*
 *----------------------------------------------------------------------
 *
 * NsTclAddBasicCmds, NsTclAddServerCmds --
 *
 *      Add basic and server Tcl commands to an interp.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
AddCmds(Cmd *cmdPtr, NsInterp *itPtr)
{
    Tcl_Interp *interp = itPtr->interp;

    while (cmdPtr->name != NULL) {
        if (cmdPtr->objProc != NULL) {
            Tcl_CreateObjCommand(interp, cmdPtr->name, cmdPtr->objProc, itPtr, NULL);
        } else {
            Tcl_CreateCommand(interp, cmdPtr->name, cmdPtr->proc, itPtr, NULL);
        }
        ++cmdPtr;
    }
}

void
NsTclAddBasicCmds(NsInterp *itPtr)
{
    AddCmds(basicCmds, itPtr);
}

void
NsTclAddServerCmds(NsInterp *itPtr)
{
    AddCmds(servCmds, itPtr);
}
