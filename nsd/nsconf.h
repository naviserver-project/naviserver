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
 * nsconf.h --
 *
 *      Default configuration values used by the core server.
 *
 *      $Header$
 */

#ifndef NSCONF_H
#define NSCONF_H


#define LOG_NOTICE_BOOL                 NS_TRUE
#define LOG_DEBUG_BOOL                  NS_FALSE
#define LOG_DEV_BOOL                    NS_FALSE
#define LOG_ROLL_BOOL                   NS_TRUE
#define LOG_USEC_BOOL                   NS_FALSE
#define LOG_EXPANDED_BOOL               NS_FALSE
#define LOG_MAXBACK_INT                 10
#define LOG_MAXLEVEL_INT                INT_MAX
#define LOG_MAXBUFFER_INT               10
#define LOG_FILE_STRING                 "server.log"

#define THREAD_STACKSIZE_INT            (64*1024)
#define SCHED_MAXELAPSED_INT            2
#define SHUTDOWNTIMEOUT_INT             20
#define SOCKLISTENBACKLOG_INT           32

#define DNS_CACHE_BOOL                  NS_TRUE
#define DNS_TIMEOUT_INT                 60

#define SERV_MAXCONNS_INT               100
#define SERV_MAXCONNSPERTHREAD_INT      0
#define SERV_MAXTHREADS_INT             10
#define SERV_MINTHREADS_INT             0
#define SERV_THREADTIMEOUT_INT          120
#define SERV_MODSINCE_BOOL              NS_TRUE
#define SERV_FLUSHCONTENT_BOOL          NS_FALSE
#define SERV_NOTICEDETAIL_BOOL          NS_TRUE
#define SERV_ERRORMINSIZE_INT           514

#define DRV_BUFSIZE_INT                 16000
#define DRV_MAXINPUT_INT                1000*1024
#define DRV_MININPUT_INT                1024
#define DRV_MAXLINE_INT                 4*1024
#define DRV_MINLINE_INT                 256
#define DRV_MAXHEADERS_INT              64*1024
#define DRV_RCVBUF_INT                  0
#define DRV_SNDBUF_INT                  0
#define DRV_RCVWAIT_INT                 30
#define DRV_SNDWAIT_INT                 30
#define DRV_KEEPWAIT_INT                30
#define DRV_CLOSEWAIT_INT               2
#define DRV_KEEPWAIT_INT                30
#define DRV_KEEPALLMETHODS_BOOL         NS_FALSE


#define FASTPATH_CACHESIZE_INT          5000*1024
#define FASTPATH_CACHEMAXENTRY_INT      8192
#define FASTPATH_MMAP_BOOL              NS_FALSE

#define VHOST_ENABLED_BOOL              NS_FALSE
#define VHOST_HASHMIN_INT               0
#define VHOST_HASHMAX_INT               5

#define ADP_CACHESIZE_INT               5000*1024
#define ADP_ENABLEEXPIRE_BOOL           NS_FALSE
#define ADP_ENABLEDEBUG_BOOL            NS_FALSE
#define ADP_DEBUGINIT_STRING            "ns_adp_debuginit"
#define ADP_DEFPARSER_STRING            "adp"
#define ADP_ENABLECOMPRESS_BOOL         NS_FALSE
#define ADP_COMPRESSLEVEL_INT           4

#define TCL_NSVBUCKETS_INT              8
#define TCL_INITLCK_BOOL                NS_FALSE


#endif
