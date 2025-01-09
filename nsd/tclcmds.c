/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
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
    TCL_OBJCMDPROC_T *objProc;
} Cmd;

/*
 * The following commands are generic, available in the config
 * and virtual server interps.
 */

static const Cmd basicCmds[] = {
#ifdef NS_WITH_DEPRECATED
    {"keyldel",                  TclX_KeyldelObjCmd},
    {"keylget",                  TclX_KeylgetObjCmd},
    {"keylkeys",                 TclX_KeylkeysObjCmd},
    {"keylset",                  TclX_KeylsetObjCmd},
#endif
    {"ns_absoluteurl",           NsTclAbsoluteUrlObjCmd},
    {"ns_addrbyhost",            NsTclGetAddrObjCmd},
    {"ns_after",                 NsTclAfterObjCmd},
    {"ns_asynclogfile",          NsTclAsyncLogfileObjCmd},
    {"ns_atexit",                NsTclAtExitObjCmd},
    {"ns_atprestartup",          NsTclAtPreStartupObjCmd},
    {"ns_atshutdown",            NsTclAtShutdownObjCmd},
    {"ns_atsignal",              NsTclAtSignalObjCmd},
    {"ns_atstartup",             NsTclAtStartupObjCmd},
    {"ns_base64decode",          NsTclBase64DecodeObjCmd},
    {"ns_base64encode",          NsTclBase64EncodeObjCmd},
    {"ns_base64urldecode",       NsTclBase64UrlDecodeObjCmd},
    {"ns_base64urlencode",       NsTclBase64UrlEncodeObjCmd},
    {"ns_baseunit" ,             NsTclBaseUnitObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_cancel",                NsTclCancelObjCmd},
#endif
    {"ns_certctl",               NsTclCertCtlObjCmd},
    {"ns_charsets",              NsTclCharsetsObjCmd},
    {"ns_config",                NsTclConfigObjCmd},
    {"ns_configsection",         NsTclConfigSectionObjCmd},
    {"ns_configsections",        NsTclConfigSectionsObjCmd},
    {"ns_crash",                 NsTclCrashObjCmd},
    {"ns_crypt",                 NsTclCryptObjCmd},
    {"ns_crypto::aead::decrypt", NsTclCryptoAeadDecryptObjCmd},
    {"ns_crypto::aead::encrypt", NsTclCryptoAeadEncryptObjCmd},
    {"ns_crypto::argon2",        NsTclCryptoArgon2ObjCmd},
    {"ns_crypto::eckey",         NsTclCryptoEckeyObjCmd},
    {"ns_crypto::hmac",          NsTclCryptoHmacObjCmd},
    {"ns_crypto::md",            NsTclCryptoMdObjCmd},
    {"ns_crypto::pbkdf2_hmac",   NsTclCryptoPbkdf2hmacObjCmd},
    {"ns_crypto::randombytes",   NsTclCryptoRandomBytesObjCmd},
    {"ns_crypto::scrypt",        NsTclCryptoScryptObjCmd},
    {"ns_encodingforcharset",    NsTclEncodingForCharsetObjCmd},
    {"ns_env",                   NsTclEnvObjCmd},
    {"ns_fastpath_cache_stats",  NsTclFastPathCacheStatsObjCmd},
    {"ns_filestat",              NsTclFileStatObjCmd},
    {"ns_fmttime",               NsTclStrftimeObjCmd},
    {"ns_fseekchars",            NsTclFSeekCharsObjCmd},
    {"ns_ftruncate",             NsTclFTruncateObjCmd},
    {"ns_getcsv",                NsTclGetCsvObjCmd},
    {"ns_gifsize",               NsTclGifSizeObjCmd},
    {"ns_gmtime",                NsTclGmTimeObjCmd},
    {"ns_guesstype",             NsTclGuessTypeObjCmd},
    {"ns_hash",                  NsTclHashObjCmd},
    {"ns_hashpath",              NsTclHashPathObjCmd},
    {"ns_hostbyaddr",            NsTclGetHostObjCmd},
    {"ns_hrefs",                 NsTclHrefsObjCmd},
    {"ns_http",                  NsTclHttpObjCmd},
    {"ns_httptime",              NsTclHttpTimeObjCmd},
    {"ns_imgmime",               NsTclImgMimeObjCmd},
    {"ns_imgsize",               NsTclImgSizeObjCmd},
    {"ns_imgtype",               NsTclImgTypeObjCmd},
    {"ns_info",                  NsTclInfoObjCmd},
    {"ns_ip",                    NsTclIpObjCmd},
    {"ns_job",                   NsTclJobObjCmd},
    {"ns_jpegsize",              NsTclJpegSizeObjCmd},
    {"ns_kill",                  NsTclKillObjCmd},
    {"ns_localtime",             NsTclLocalTimeObjCmd},
    {"ns_locationproc",          NsTclLocationProcObjCmd},
    {"ns_log",                   NsTclLogObjCmd},
    {"ns_logctl",                NsTclLogCtlObjCmd},
    {"ns_logroll",               NsTclLogRollObjCmd},
    {"ns_md5",                   NsTclMD5ObjCmd},
    {"ns_mkdtemp",               NsTclMkdTempObjCmd},
    {"ns_mktemp",                NsTclMkTempObjCmd},
    {"ns_modulepath",            NsTclModulePathObjCmd},
    {"ns_normalizepath",         NsTclNormalizePathObjCmd},
    {"ns_pagepath",              NsTclPagePathObjCmd},
    {"ns_parseargs",             NsTclParseArgsObjCmd},
    {"ns_parsefieldvalue",       NsTclParseFieldvalue},
    {"ns_parseheader",           NsTclParseHeaderObjCmd},
    {"ns_parsehostport",         NsTclParseHostportObjCmd},
    {"ns_parsehttptime",         NsTclParseHttpTimeObjCmd},
    {"ns_parsemessage",          NsTclParseMessageObjCmd},
    {"ns_parseurl",              NsTclParseUrlObjCmd},
    {"ns_pause",                 NsTclPauseObjCmd},
    {"ns_pngsize",               NsTclPngSizeObjCmd},
    {"ns_purgefiles",            NsTclPurgeFilesObjCmd},
    {"ns_quotehtml",             NsTclQuoteHtmlObjCmd},
    {"ns_rand",                  NsTclRandObjCmd},
    {"ns_resume",                NsTclResumeObjCmd},
    {"ns_rlimit",                NsTclRlimitObjCmd},
    {"ns_rollfile",              NsTclRollFileObjCmd},
    {"ns_schedule_daily",        NsTclSchedDailyObjCmd},
    {"ns_schedule_proc",         NsTclSchedObjCmd},
    {"ns_schedule_weekly",       NsTclSchedWeeklyObjCmd},
    {"ns_serverpath",            NsTclServerPathObjCmd},
    {"ns_serverrootproc",        NsTclServerRootProcObjCmd},
    {"ns_set",                   NsTclSetObjCmd},
    {"ns_sha1",                  NsTclSHA1ObjCmd},
    {"ns_shortcut_filter",       NsTclShortcutFilterObjCmd},
    {"ns_sleep",                 NsTclSleepObjCmd},
    {"ns_sls",                   NsTclSlsObjCmd},
    {"ns_sockaccept",            NsTclSockAcceptObjCmd},
    {"ns_sockblocking",          NsTclSockSetBlockingObjCmd},
    {"ns_sockcallback",          NsTclSockCallbackObjCmd},
    {"ns_sockcheck",             NsTclSockCheckObjCmd},
    {"ns_socketpair",            NsTclSocketPairObjCmd},
    {"ns_socklisten",            NsTclSockListenObjCmd},
    {"ns_socklistencallback",    NsTclSockListenCallbackObjCmd},
    {"ns_socknonblocking",       NsTclSockSetNonBlockingObjCmd},
    {"ns_socknread",             NsTclSockNReadObjCmd},
    {"ns_sockopen",              NsTclSockOpenObjCmd},
    {"ns_sockselect",            NsTclSelectObjCmd},
    {"ns_strcoll",               NsTclStrcollObjCmd},
    {"ns_striphtml",             NsTclStripHtmlObjCmd},
    {"ns_parsehtml",             NsTclParseHtmlObjCmd},
    {"ns_shutdown",              NsTclShutdownObjCmd},
    {"ns_symlink",               NsTclSymlinkObjCmd},
    {"ns_thread",                NsTclThreadObjCmd},
    {"ns_time",                  NsTclTimeObjCmd},
    {"ns_truncate",              NsTclTruncateObjCmd},
    {"ns_unquotehtml",           NsTclUnquoteHtmlObjCmd},
    {"ns_unschedule_proc",       NsTclUnscheduleObjCmd},
    {"ns_urldecode",             NsTclUrlDecodeObjCmd},
    {"ns_urlencode",             NsTclUrlEncodeObjCmd},
    {"ns_uudecode",              NsTclBase64DecodeObjCmd},
    {"ns_uuencode",              NsTclBase64EncodeObjCmd},
    {"ns_valid_utf8",            NsTclValidUtf8ObjCmd},
    {"ns_writefp",               NsTclWriteFpObjCmd},
    {NULL, NULL}
};

/*
 * The following commands require the NsServer context and
 * are available only in virtual server interps.
 */

static const Cmd servCmds[] = {
    {"_ns_adp_include",          NsTclAdpIncludeObjCmd},
    {"ns_adp_abort",             NsTclAdpAbortObjCmd},
    {"ns_adp_append",            NsTclAdpAppendObjCmd},
    {"ns_adp_argc",              NsTclAdpArgcObjCmd},
    {"ns_adp_argv",              NsTclAdpArgvObjCmd},
    {"ns_adp_bind_args",         NsTclAdpBindArgsObjCmd},
    {"ns_adp_break",             NsTclAdpBreakObjCmd},
    {"ns_adp_close",             NsTclAdpCloseObjCmd},
    {"ns_adp_ctl",               NsTclAdpCtlObjCmd},
    {"ns_adp_debug",             NsTclAdpDebugObjCmd},
    {"ns_adp_dir",               NsTclAdpDirObjCmd},
    {"ns_adp_dump",              NsTclAdpDumpObjCmd},
    {"ns_adp_exception",         NsTclAdpExceptionObjCmd},
    {"ns_adp_flush",             NsTclAdpFlushObjCmd},
    {"ns_adp_info",              NsTclAdpInfoObjCmd},
    {"ns_adp_mimetype",          NsTclAdpMimeTypeObjCmd},
    {"ns_adp_parse",             NsTclAdpParseObjCmd},
    {"ns_adp_puts",              NsTclAdpPutsObjCmd},
    {"ns_adp_registeradp",       NsTclAdpRegisterAdpObjCmd},
    {"ns_adp_registerproc",      NsTclAdpRegisterProcObjCmd},
    {"ns_adp_registerscript",    NsTclAdpRegisterScriptObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_adp_registertag",       NsTclAdpRegisterTagObjCmd},
#endif
    {"ns_adp_return",            NsTclAdpReturnObjCmd},
    {"ns_adp_stats",             NsTclAdpStatsObjCmd},
    {"ns_adp_tell",              NsTclAdpTellObjCmd},
    {"ns_adp_trunc",             NsTclAdpTruncObjCmd},
    {"ns_atclose",               NsTclAtCloseObjCmd},
    {"ns_cache_append",          NsTclCacheAppendObjCmd},
    {"ns_cache_configure",       NsTclCacheConfigureObjCmd},
    {"ns_cache_create",          NsTclCacheCreateObjCmd},
    {"ns_cache_eval",            NsTclCacheEvalObjCmd},
    {"ns_cache_exists",          NsTclCacheExistsObjCmd},
    {"ns_cache_flush",           NsTclCacheFlushObjCmd},
    {"ns_cache_get",             NsTclCacheGetObjCmd},
    {"ns_cache_incr",            NsTclCacheIncrObjCmd},
    {"ns_cache_keys",            NsTclCacheKeysObjCmd},
    {"ns_cache_lappend",         NsTclCacheLappendObjCmd},
    {"ns_cache_names",           NsTclCacheNamesObjCmd},
    {"ns_cache_stats",           NsTclCacheStatsObjCmd},
    {"ns_cache_transaction_begin", NsTclCacheTransactionBeginObjCmd},
    {"ns_cache_transaction_commit", NsTclCacheTransactionCommitObjCmd},
    {"ns_cache_transaction_rollback", NsTclCacheTransactionRollbackObjCmd},
    {"ns_chan",                  NsTclChanObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_checkurl",              NsTclRequestAuthorizeObjCmd},
#endif
    {"ns_cond",                  NsTclCondObjCmd},
    {"ns_conn",                  NsTclConnObjCmd},
    {"ns_connchan",              NsTclConnChanObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_conncptofp",            NsTclWriteContentObjCmd},
    {"ns_connsendfp",            NsTclConnSendFpObjCmd},
#endif
    {"ns_critsec",               NsTclCritSecObjCmd},
    {"ns_deletecookie",          NsTclDeleteCookieObjCmd},
    {"ns_driver",                NsTclDriverObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_event",                 NsTclCondObjCmd},
#endif
    {"ns_getcookie",             NsTclGetCookieObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_geturl",                NsTclGetUrlObjCmd},
#endif
    {"ns_headers",               NsTclHeadersObjCmd},
    {"ns_ictl",                  NsTclICtlObjCmd},
    {"ns_internalredirect",      NsTclInternalRedirectObjCmd},
    {"ns_library",               NsTclLibraryObjCmd},
    {"ns_limits_get",            NsTclGetLimitsObjCmd},
    {"ns_limits_list",           NsTclListLimitsObjCmd},
    {"ns_limits_register",       NsTclRegisterLimitsObjCmd},
    {"ns_limits_set",            NsTclSetLimitsObjCmd},
    {"ns_moduleload",            NsTclModuleLoadObjCmd},
    {"ns_mutex",                 NsTclMutexObjCmd},
    {"ns_normalizepath",         NsTclNormalizePathObjCmd},
    {"ns_parsequery",            NsTclParseQueryObjCmd},
    {"ns_reflow_text",           NsTclReflowTextObjCmd},
    {"ns_register_adp",          NsTclRegisterAdpObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_register_adptag",       NsTclAdpRegisterAdptagObjCmd},
#endif
    {"ns_register_fastpath",     NsTclRegisterFastPathObjCmd},
    {"ns_register_fasturl2file", NsTclRegisterFastUrl2FileObjCmd},
    {"ns_register_filter",       NsTclRegisterFilterObjCmd},
    {"ns_register_proc",         NsTclRegisterProcObjCmd},
    {"ns_register_proxy",        NsTclRegisterProxyObjCmd},
    {"ns_register_tcl",          NsTclRegisterTclObjCmd},
    {"ns_register_trace",        NsTclRegisterTraceObjCmd},
    {"ns_register_url2file",     NsTclRegisterUrl2FileObjCmd},
    {"ns_requestauthorize",      NsTclRequestAuthorizeObjCmd},
    {"ns_respond",               NsTclRespondObjCmd},
    {"ns_return",                NsTclReturnObjCmd},
    {"ns_returnbadrequest",      NsTclReturnBadRequestObjCmd},
    {"ns_returnerror",           NsTclReturnErrorObjCmd},
    {"ns_returnfile",            NsTclReturnFileObjCmd},
    {"ns_returnforbidden",       NsTclReturnForbiddenObjCmd},
    {"ns_returnfp",              NsTclReturnFpObjCmd},
    {"ns_returnmoved",           NsTclReturnMovedObjCmd},
    {"ns_returnnotfound",        NsTclReturnNotFoundObjCmd},
    {"ns_returnnotice",          NsTclReturnNoticeObjCmd},
    {"ns_returnredirect",        NsTclReturnRedirectObjCmd},
    {"ns_returnunauthorized",    NsTclReturnUnauthorizedObjCmd},
    {"ns_returnunavailable",     NsTclReturnUnavailableObjCmd},
    {"ns_runonce",               NsTclRunOnceObjCmd},
    {"ns_rwlock",                NsTclRWLockObjCmd},
    {"ns_sema",                  NsTclSemaObjCmd},
    {"ns_server",                NsTclServerObjCmd},
    {"ns_setcookie",             NsTclSetCookieObjCmd},
    {"ns_setgroup",              NsTclSetGroupObjCmd},
    {"ns_setuser",               NsTclSetUserObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_startcontent",          NsTclStartContentObjCmd},
#endif
    {"ns_trim",                  NsTclTrimObjCmd},
    {"ns_unregister_op",         NsTclUnRegisterOpObjCmd},
    {"ns_unregister_url2file",   NsTclUnRegisterUrl2FileObjCmd},
    {"ns_upload_stats",          NsTclProgressObjCmd},
    {"ns_url2file",              NsTclUrl2FileObjCmd},
    {"ns_urlspace",              NsTclUrlSpaceObjCmd},
    {"ns_write",                 NsTclWriteObjCmd},
#ifdef NS_WITH_DEPRECATED
    {"ns_writecontent",          NsTclWriteContentObjCmd},
#endif
    {"ns_writer",                NsTclWriterObjCmd},
    {"nsv_append",               NsTclNsvAppendObjCmd},
    {"nsv_array",                NsTclNsvArrayObjCmd},
    {"nsv_dict",                 NsTclNsvDictObjCmd},
    {"nsv_bucket",               NsTclNsvBucketObjCmd},
    {"nsv_exists",               NsTclNsvExistsObjCmd},
    {"nsv_get",                  NsTclNsvGetObjCmd},
    {"nsv_incr",                 NsTclNsvIncrObjCmd},
    {"nsv_lappend",              NsTclNsvLappendObjCmd},
    {"nsv_names",                NsTclNsvNamesObjCmd},
    {"nsv_set",                  NsTclNsvSetObjCmd},
    {"nsv_unset",                NsTclNsvUnsetObjCmd},
    /*
     * Add more server Tcl commands here.
     */

    {NULL, NULL}
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
        if (cmdPtr->objProc != NULL) {
            (void)TCL_CREATEOBJCOMMAND(itPtr->interp, cmdPtr->name, cmdPtr->objProc, itPtr, NULL);
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
