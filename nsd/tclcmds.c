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
 * tclcmds.c --
 *
 *      Connect Tcl command names to the functions that implement them.
 */

#include "nsd.h"

/*
 * The following structure defines a command to be created
 * in new interps.
 */

typedef struct Cmd {
    const char *name;
    Tcl_CmdProc *proc;
    Tcl_ObjCmdProc *objProc;
} Cmd;

/*
 * The following commands are generic, available in the config
 * and virtual server interps.
 */

static const Cmd basicCmds[] = {
    {"keyldel",                  NULL, TclX_KeyldelObjCmd},
    {"keylget",                  NULL, TclX_KeylgetObjCmd},
    {"keylkeys",                 NULL, TclX_KeylkeysObjCmd},
    {"keylset",                  NULL, TclX_KeylsetObjCmd},
    {"ns_absoluteurl",           NULL, NsTclAbsoluteUrlObjCmd},
    {"ns_addrbyhost",            NULL, NsTclGetAddrObjCmd},
    {"ns_after",                 NULL, NsTclAfterObjCmd},
    {"ns_asynclogfile",          NULL, NsTclAsyncLogfileObjCmd},
    {"ns_atexit",                NULL, NsTclAtExitObjCmd},
    {"ns_atprestartup",          NULL, NsTclAtPreStartupObjCmd},
    {"ns_atshutdown",            NULL, NsTclAtShutdownObjCmd},
    {"ns_atsignal",              NULL, NsTclAtSignalObjCmd},
    {"ns_atstartup",             NULL, NsTclAtStartupObjCmd},
    {"ns_base64decode",          NULL, NsTclBase64DecodeObjCmd},
    {"ns_base64encode",          NULL, NsTclBase64EncodeObjCmd},
    {"ns_base64urldecode",       NULL, NsTclBase64UrlDecodeObjCmd},
    {"ns_base64urlencode",       NULL, NsTclBase64UrlEncodeObjCmd},
    {"ns_baseunit" ,             NULL, NsTclBaseUnitObjCmd},
    {"ns_cancel",                NULL, NsTclCancelObjCmd},
    {"ns_charsets",              NULL, NsTclCharsetsObjCmd},
    {"ns_config",                NULL, NsTclConfigObjCmd},
    {"ns_configsection",         NULL, NsTclConfigSectionObjCmd},
    {"ns_configsections",        NULL, NsTclConfigSectionsObjCmd},
    {"ns_crash",                 NULL, NsTclCrashObjCmd},
    {"ns_crypt",                 NULL, NsTclCryptObjCmd},
    {"ns_crypto::aead::decrypt", NULL, NsTclCryptoAeadDecryptObjCmd},
    {"ns_crypto::aead::encrypt", NULL, NsTclCryptoAeadEncryptObjCmd},
    {"ns_crypto::eckey",         NULL, NsTclCryptoEckeyObjCmd},
    {"ns_crypto::hmac",          NULL, NsTclCryptoHmacObjCmd},
    {"ns_crypto::md",            NULL, NsTclCryptoMdObjCmd},
    {"ns_crypto::pbkdf2_hmac",   NULL, NsTclCryptoPbkdf2hmacObjCmd},
    {"ns_crypto::randombytes",   NULL, NsTclCryptoRandomBytesObjCmd},
    {"ns_crypto::scrypt",        NULL, NsTclCryptoScryptObjCmd},
    {"ns_encodingforcharset",    NULL, NsTclEncodingForCharsetObjCmd},
    {"ns_env",                   NULL, NsTclEnvObjCmd},
    {"ns_fastpath_cache_stats",  NULL, NsTclFastPathCacheStatsObjCmd},
    {"ns_filestat",              NULL, NsTclFileStatObjCmd},
    {"ns_fmttime",               NULL, NsTclStrftimeObjCmd},
    {"ns_ftruncate",             NULL, NsTclFTruncateObjCmd},
    {"ns_gifsize",               NULL, NsTclGifSizeObjCmd},
    {"ns_gmtime",                NULL, NsTclGmTimeObjCmd},
    {"ns_guesstype",             NULL, NsTclGuessTypeObjCmd},
    {"ns_hash",                  NULL, NsTclHashObjCmd},
    {"ns_hashpath",              NULL, NsTclHashPathObjCmd},
    {"ns_hostbyaddr",            NULL, NsTclGetHostObjCmd},
    {"ns_hrefs",                 NULL, NsTclHrefsObjCmd},
    {"ns_http",                  NULL, NsTclHttpObjCmd},
    {"ns_httptime",              NULL, NsTclHttpTimeObjCmd},
    {"ns_imgmime",               NULL, NsTclImgMimeObjCmd},
    {"ns_imgsize",               NULL, NsTclImgSizeObjCmd},
    {"ns_imgtype",               NULL, NsTclImgTypeObjCmd},
    {"ns_info",                  NULL, NsTclInfoObjCmd},
    {"ns_job",                   NULL, NsTclJobObjCmd},
    {"ns_jpegsize",              NULL, NsTclJpegSizeObjCmd},
    {"ns_kill",                  NULL, NsTclKillObjCmd},
    {"ns_localtime",             NULL, NsTclLocalTimeObjCmd},
    {"ns_locationproc",          NULL, NsTclLocationProcObjCmd},
    {"ns_log",                   NULL, NsTclLogObjCmd},
    {"ns_logctl",                NULL, NsTclLogCtlObjCmd},
    {"ns_logroll",               NULL, NsTclLogRollObjCmd},
    {"ns_md5",                   NULL, NsTclMD5ObjCmd},
    {"ns_mktemp",                NULL, NsTclMkTempObjCmd},
    {"ns_modulepath",            NULL, NsTclModulePathObjCmd},
    {"ns_normalizepath",         NULL, NsTclNormalizePathObjCmd},
    {"ns_pagepath",              NULL, NsTclPagePathObjCmd},
    {"ns_parseargs",             NULL, NsTclParseArgsObjCmd},
    {"ns_parsefieldvalue",       NULL, NsTclParseFieldvalue},
    {"ns_parseheader",           NULL, NsTclParseHeaderObjCmd},
    {"ns_parsehostport",         NULL, NsTclParseHostportObjCmd},
    {"ns_parsehttptime",         NULL, NsTclParseHttpTimeObjCmd},
    {"ns_parsequery",            NULL, NsTclParseQueryObjCmd},
    {"ns_parseurl",              NULL, NsTclParseUrlObjCmd},
    {"ns_pause",                 NULL, NsTclPauseObjCmd},
    {"ns_pngsize",               NULL, NsTclPngSizeObjCmd},
    {"ns_purgefiles",            NULL, NsTclPurgeFilesObjCmd},
    {"ns_quotehtml",             NULL, NsTclQuoteHtmlObjCmd},
    {"ns_rand",                  NULL, NsTclRandObjCmd},
    {"ns_resume",                NULL, NsTclResumeObjCmd},
    {"ns_rlimit",                NULL, NsTclRlimitObjCmd},
    {"ns_rollfile",              NULL, NsTclRollFileObjCmd},
    {"ns_schedule_daily",        NULL, NsTclSchedDailyObjCmd},
    {"ns_schedule_proc",         NULL, NsTclSchedObjCmd},
    {"ns_schedule_weekly",       NULL, NsTclSchedWeeklyObjCmd},
    {"ns_serverpath",            NULL, NsTclServerPathObjCmd},
    {"ns_serverrootproc",        NULL, NsTclServerRootProcObjCmd},
    {"ns_set",                   NULL, NsTclSetObjCmd},
    {"ns_sha1",                  NULL, NsTclSHA1ObjCmd},
    {"ns_shortcut_filter",       NULL, NsTclShortcutFilterObjCmd},
    {"ns_sleep",                 NULL, NsTclSleepObjCmd},
    {"ns_sls",                   NULL, NsTclSlsObjCmd},
    {"ns_sockaccept",            NULL, NsTclSockAcceptObjCmd},
    {"ns_sockblocking",          NULL, NsTclSockSetBlockingObjCmd},
    {"ns_sockcallback",          NULL, NsTclSockCallbackObjCmd},
    {"ns_sockcheck",             NULL, NsTclSockCheckObjCmd},
    {"ns_socketpair",            NULL, NsTclSocketPairObjCmd},
    {"ns_socklisten",            NULL, NsTclSockListenObjCmd},
    {"ns_socklistencallback",    NULL, NsTclSockListenCallbackObjCmd},
    {"ns_socknonblocking",       NULL, NsTclSockSetNonBlockingObjCmd},
    {"ns_socknread",             NULL, NsTclSockNReadObjCmd},
    {"ns_sockopen",              NULL, NsTclSockOpenObjCmd},
    {"ns_sockselect",            NULL, NsTclSelectObjCmd},
    {"ns_strcoll",               NULL, NsTclStrcollObjCmd},
    {"ns_striphtml",             NULL, NsTclStripHtmlObjCmd},
    {"ns_symlink",               NULL, NsTclSymlinkObjCmd},
    {"ns_thread",                NULL, NsTclThreadObjCmd},
    {"ns_time",                  NULL, NsTclTimeObjCmd},
    {"ns_truncate",              NULL, NsTclTruncateObjCmd},
    {"ns_unschedule_proc",       NULL, NsTclUnscheduleObjCmd},
    {"ns_urldecode",             NULL, NsTclUrlDecodeObjCmd},
    {"ns_urlencode",             NULL, NsTclUrlEncodeObjCmd},
    {"ns_uudecode",              NULL, NsTclBase64DecodeObjCmd},
    {"ns_uuencode",              NULL, NsTclBase64EncodeObjCmd},
    {"ns_valid_utf8",            NULL, NsTclValidUtf8ObjCmd},
    {"ns_writefp",               NULL, NsTclWriteFpObjCmd},
    {NULL, NULL, NULL}
};

/*
 * The following commands require the NsServer context and
 * are available only in virtual server interps.
 */

static const Cmd servCmds[] = {
    {"_ns_adp_include",          NULL, NsTclAdpIncludeObjCmd},
    {"ns_adp_abort",             NULL, NsTclAdpAbortObjCmd},
    {"ns_adp_append",            NULL, NsTclAdpAppendObjCmd},
    {"ns_adp_argc",              NULL, NsTclAdpArgcObjCmd},
    {"ns_adp_argv",              NULL, NsTclAdpArgvObjCmd},
    {"ns_adp_bind_args",         NULL, NsTclAdpBindArgsObjCmd},
    {"ns_adp_break",             NULL, NsTclAdpBreakObjCmd},
    {"ns_adp_close",             NULL, NsTclAdpCloseObjCmd},
    {"ns_adp_ctl",               NULL, NsTclAdpCtlObjCmd},
    {"ns_adp_debug",             NULL, NsTclAdpDebugObjCmd},
    {"ns_adp_dir",               NULL, NsTclAdpDirObjCmd},
    {"ns_adp_dump",              NULL, NsTclAdpDumpObjCmd},
    {"ns_adp_exception",         NULL, NsTclAdpExceptionObjCmd},
    {"ns_adp_flush",             NULL, NsTclAdpFlushObjCmd},
    {"ns_adp_info",              NULL, NsTclAdpInfoObjCmd},
    {"ns_adp_mimetype",          NULL, NsTclAdpMimeTypeObjCmd},
    {"ns_adp_parse",             NULL, NsTclAdpParseObjCmd},
    {"ns_adp_puts",              NULL, NsTclAdpPutsObjCmd},
    {"ns_adp_registeradp",       NULL, NsTclAdpRegisterAdpObjCmd},
    {"ns_adp_registerproc",      NULL, NsTclAdpRegisterProcObjCmd},
    {"ns_adp_registerscript",    NULL, NsTclAdpRegisterScriptObjCmd},
    {"ns_adp_registertag",       NULL, NsTclAdpRegisterTagObjCmd},
    {"ns_adp_return",            NULL, NsTclAdpReturnObjCmd},
    {"ns_adp_stats",             NULL, NsTclAdpStatsObjCmd},
    {"ns_adp_tell",              NULL, NsTclAdpTellObjCmd},
    {"ns_adp_trunc",             NULL, NsTclAdpTruncObjCmd},
    {"ns_atclose",               NULL, NsTclAtCloseObjCmd},
    {"ns_cache_append",          NULL, NsTclCacheAppendObjCmd},
    {"ns_cache_configure",       NULL, NsTclCacheConfigureObjCmd},
    {"ns_cache_create",          NULL, NsTclCacheCreateObjCmd},
    {"ns_cache_eval",            NULL, NsTclCacheEvalObjCmd},
    {"ns_cache_exists",          NULL, NsTclCacheExistsObjCmd},
    {"ns_cache_flush",           NULL, NsTclCacheFlushObjCmd},
    {"ns_cache_get",             NULL, NsTclCacheGetObjCmd},
    {"ns_cache_incr",            NULL, NsTclCacheIncrObjCmd},
    {"ns_cache_keys",            NULL, NsTclCacheKeysObjCmd},
    {"ns_cache_lappend",         NULL, NsTclCacheLappendObjCmd},
    {"ns_cache_names",           NULL, NsTclCacheNamesObjCmd},
    {"ns_cache_stats",           NULL, NsTclCacheStatsObjCmd},
    {"ns_cache_transaction_begin", NULL, NsTclCacheTransactionBeginObjCmd},
    {"ns_cache_transaction_commit", NULL, NsTclCacheTransactionCommitObjCmd},
    {"ns_cache_transaction_rollback", NULL, NsTclCacheTransactionRollbackObjCmd},
    {"ns_chan",                  NULL, NsTclChanObjCmd},
    {"ns_checkurl",              NULL, NsTclRequestAuthorizeObjCmd},
    {"ns_cond",                  NULL, NsTclCondObjCmd},
    {"ns_conn",                  NULL, NsTclConnObjCmd},
    {"ns_connchan",              NULL, NsTclConnChanObjCmd},
    {"ns_conncptofp",            NULL, NsTclWriteContentObjCmd},
    {"ns_connsendfp",            NULL, NsTclConnSendFpObjCmd},
    {"ns_critsec",               NULL, NsTclCritSecObjCmd},
    {"ns_deletecookie",          NULL, NsTclDeleteCookieObjCmd},
    {"ns_driver",                NULL, NsTclDriverObjCmd},
    {"ns_event",                 NULL, NsTclCondObjCmd},
    {"ns_getcookie",             NULL, NsTclGetCookieObjCmd},
    {"ns_geturl",                NULL, NsTclGetUrlObjCmd},
    {"ns_headers",               NULL, NsTclHeadersObjCmd},
    {"ns_ictl",                  NULL, NsTclICtlObjCmd},
    {"ns_internalredirect",      NULL, NsTclInternalRedirectObjCmd},
    {"ns_library",               NULL, NsTclLibraryObjCmd},
    {"ns_limits_get",            NULL, NsTclGetLimitsObjCmd},
    {"ns_limits_list",           NULL, NsTclListLimitsObjCmd},
    {"ns_limits_register",       NULL, NsTclRegisterLimitsObjCmd},
    {"ns_limits_set",            NULL, NsTclSetLimitsObjCmd},
    {"ns_moduleload",            NULL, NsTclModuleLoadObjCmd},
    {"ns_mutex",                 NULL, NsTclMutexObjCmd},
    {"ns_normalizepath",         NULL, NsTclNormalizePathObjCmd},
    {"ns_reflow_text",           NULL, NsTclReflowTextObjCmd},
    {"ns_register_adp",          NULL, NsTclRegisterAdpObjCmd},
    {"ns_register_adptag",       NULL, NsTclAdpRegisterAdptagObjCmd},
    {"ns_register_fastpath",     NULL, NsTclRegisterFastPathObjCmd},
    {"ns_register_fasturl2file", NULL, NsTclRegisterFastUrl2FileObjCmd},
    {"ns_register_filter",       NULL, NsTclRegisterFilterObjCmd},
    {"ns_register_proc",         NULL, NsTclRegisterProcObjCmd},
    {"ns_register_proxy",        NULL, NsTclRegisterProxyObjCmd},
    {"ns_register_tcl",          NULL, NsTclRegisterTclObjCmd},
    {"ns_register_trace",        NULL, NsTclRegisterTraceObjCmd},
    {"ns_register_url2file",     NULL, NsTclRegisterUrl2FileObjCmd},
    {"ns_requestauthorize",      NULL, NsTclRequestAuthorizeObjCmd},
    {"ns_respond",               NULL, NsTclRespondObjCmd},
    {"ns_return",                NULL, NsTclReturnObjCmd},
    {"ns_returnbadrequest",      NULL, NsTclReturnBadRequestObjCmd},
    {"ns_returnerror",           NULL, NsTclReturnErrorObjCmd},
    {"ns_returnfile",            NULL, NsTclReturnFileObjCmd},
    {"ns_returnforbidden",       NULL, NsTclReturnForbiddenObjCmd},
    {"ns_returnfp",              NULL, NsTclReturnFpObjCmd},
    {"ns_returnmoved",           NULL, NsTclReturnMovedObjCmd},
    {"ns_returnnotfound",        NULL, NsTclReturnNotFoundObjCmd},
    {"ns_returnnotice",          NULL, NsTclReturnNoticeObjCmd},
    {"ns_returnredirect",        NULL, NsTclReturnRedirectObjCmd},
    {"ns_returnunauthorized",    NULL, NsTclReturnUnauthorizedObjCmd},
    {"ns_returnunavailable",     NULL, NsTclReturnUnavailableObjCmd},
    {"ns_runonce",               NULL, NsTclRunOnceObjCmd},
    {"ns_rwlock",                NULL, NsTclRWLockObjCmd},
    {"ns_sema",                  NULL, NsTclSemaObjCmd},
    {"ns_server",                NULL, NsTclServerObjCmd},
    {"ns_setcookie",             NULL, NsTclSetCookieObjCmd},
    {"ns_setgroup",              NULL, NsTclSetGroupObjCmd},
    {"ns_setuser",               NULL, NsTclSetUserObjCmd},
    {"ns_shutdown",              NULL, NsTclShutdownObjCmd},
    {"ns_startcontent",          NULL, NsTclStartContentObjCmd},
    {"ns_trim",                  NULL, NsTclTrimObjCmd},
    {"ns_unregister_op",         NULL, NsTclUnRegisterOpObjCmd},
    {"ns_unregister_url2file",   NULL, NsTclUnRegisterUrl2FileObjCmd},
    {"ns_upload_stats",          NULL, NsTclProgressObjCmd},
    {"ns_url2file",              NULL, NsTclUrl2FileObjCmd},
    {"ns_urlspace",              NULL, NsTclUrlSpaceObjCmd},
    {"ns_write",                 NULL, NsTclWriteObjCmd},
    {"ns_writecontent",          NULL, NsTclWriteContentObjCmd},
    {"ns_writer",                NULL, NsTclWriterObjCmd},
    {"nsv_append",               NULL, NsTclNsvAppendObjCmd},
    {"nsv_array",                NULL, NsTclNsvArrayObjCmd},
    {"nsv_dict",                 NULL, NsTclNsvDictObjCmd},
    {"nsv_bucket",               NULL, NsTclNsvBucketObjCmd},
    {"nsv_exists",               NULL, NsTclNsvExistsObjCmd},
    {"nsv_get",                  NULL, NsTclNsvGetObjCmd},
    {"nsv_incr",                 NULL, NsTclNsvIncrObjCmd},
    {"nsv_lappend",              NULL, NsTclNsvLappendObjCmd},
    {"nsv_names",                NULL, NsTclNsvNamesObjCmd},
    {"nsv_set",                  NULL, NsTclNsvSetObjCmd},
    {"nsv_unset",                NULL, NsTclNsvUnsetObjCmd},
    /*
     * Add more server Tcl commands here.
     */

    {NULL, NULL, NULL}
};

/*
 * Locally defined functions.
 */
static void AddCmds(const Cmd *cmdPtr, NsInterp *itPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 *
 * AddCmds --
 *
 *      Add an array of commands or objCommands to the passed
 *      interpreter.  The array is terminated by an entry with
 *      name == NULL.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      registered commands.
 *
 *----------------------------------------------------------------------
 */

static void
AddCmds(const Cmd *cmdPtr, NsInterp *itPtr)
{
    NS_NONNULL_ASSERT(cmdPtr != NULL);
    NS_NONNULL_ASSERT(itPtr != NULL);

    while (cmdPtr->name != NULL) {
        /*
         * One has to provide either an objProc or a proc.
         */
        if (cmdPtr->objProc != NULL) {
            (void)Tcl_CreateObjCommand(itPtr->interp, cmdPtr->name, cmdPtr->objProc, itPtr, NULL);
        } else {
            assert(cmdPtr->proc != NULL);
            (void)Tcl_CreateCommand(itPtr->interp, cmdPtr->name, cmdPtr->proc, itPtr, NULL);
        }
        ++cmdPtr;
    }
}

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

void
NsTclAddBasicCmds(NsInterp *itPtr)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    AddCmds(basicCmds, itPtr);
}

void
NsTclAddServerCmds(NsInterp *itPtr)
{
    NS_NONNULL_ASSERT(itPtr != NULL);

    AddCmds(servCmds, itPtr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
