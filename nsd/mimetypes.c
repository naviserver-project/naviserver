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
 * mimetypes.c --
 *
 *      Defines standard default mime types.
 */

#include "nsd.h"

#define TYPE_DEFAULT "*/*"


/*
 * Local functions defined in this file.
 */

static void AddType(CONST char *ext, CONST char *type);
static char *LowerDString(Ns_DString *dsPtr, CONST char *ext);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable    types;
static char            *defaultType = TYPE_DEFAULT;
static char            *noextType = TYPE_DEFAULT;
/*
 * The default extension matching table.  This should be kept up to date with
 * the client.  Case in the extension is ignored.
 */

static const struct exttype {
    const char     *ext;
    const char     *type;
} typetab[] = {

    /*
     * Basic text/html types.
     */
    { ".adp",   NSD_TEXTHTML},
    { ".dci",   NSD_TEXTHTML},
    { ".htm",   NSD_TEXTHTML},
    { ".html",  NSD_TEXTHTML},
    { ".sht",   NSD_TEXTHTML},
    { ".shtml", NSD_TEXTHTML},

    /*
     * All other types.
     */
    { ".323",     "text/h323" },
    { ".3dm",     "x-world/x-3dmf" },
    { ".3dmf",    "x-world/x-3dmf" },
    { ".3g2",     "video/3gpp2" },
    { ".3gp",     "video/3gpp" },
    { ".7z",      "application/x-7z-compressed" },
    { ".aab",     "application/x-authorware-bin" },
    { ".aac",     "audio/x-aac"},                          /* Wikipedia: AAC */
    { ".aam",     "application/x-authorware-map" },
    { ".aas",     "application/x-authorware-seg" },
    { ".abc",     "text/vnd.abc" },
    { ".ac",      "application/pkix-attr-cert"},           /* RFC 5877 */
    { ".acgi",    "text/html" },
    { ".acx",     "application/internet-property-stream" },
    { ".afl",     "video/animaflex" },
    { ".ai",      "application/postscript" },
    { ".aif",     "audio/aiff" },
    { ".aifc",    "audio/aiff" },
    { ".aiff",    "audio/aiff" },
    { ".aim",     "application/x-aim" },
    { ".aip",     "text/x-audiosoft-intra" },
    { ".ani",     "application/x-navi-animation" },
    { ".anx",     "application/annodex" },
    { ".aos",     "application/x-nokia-9000-communicator-add-on-software" },
    { ".aps",     "application/mime" },
    { ".arc",     "application/octet-stream" },
    { ".arj",     "application/arj" },
    { ".art",     "image/x-art" },
    { ".asf",     "video/x-ms-asf" },
    { ".asm",     "text/x-asm" },
    { ".asp",     "text/asp" },
    { ".asr",     "video/x-ms-asf" },
    { ".asx",     "video/x-ms-asf" },
    { ".atom",    "application/atom+xml" },
    { ".atomcat", "application/atomcat+xml"},              /* RFC 5023 */
    { ".atomsvc", "application/atomsvc+xml"},              /* RFC 5023 */
    { ".au",      "audio/basic" },
    { ".avi",     "video/x-msvideo" },
    { ".avs",     "video/avs-video" },
    { ".axa",     "audio/annodex" },
    { ".axs",     "application/olescript" },
    { ".axv",     "video/annodex" },
    { ".bas",     "text/plain" },
    { ".bcpio",   "application/x-bcpio" },
    { ".bin",     "application/x-macbinary" },
    { ".bm",      "image/bmp" },
    { ".bmp",     "image/bmp" },
    { ".boo",     "application/book" },
    { ".book",    "application/book" },
    { ".boz",     "application/x-bzip2" },
    { ".bsh",     "application/x-bsh" },
    { ".btif",    "image/prs.btif"},                       /* IANA: BTIF */
    { ".bz",      "application/x-bzip" },
    { ".bz2",     "application/x-bzip2" },
    { ".c",       "text/plain" },
    { ".c++",     "text/plain" },
    { ".cat",     "application/vnd.ms-pki.secret" },
    { ".cc",      "text/plain" },
    { ".ccad",    "application/clariscad" },
    { ".cco",     "application/x-cocoa" },
    { ".cdf",     "application/x-cdf" },
    { ".cdmia",   "application/cdmi-capability"},          /* RFC 6208 */
    { ".cdmic",   "application/cdmi-container"},           /* RFC 6209 */
    { ".cdmid",   "application/cdmi-domain"},              /* RFC 6210 */
    { ".cdmio",   "application/cdmi-object"},              /* RFC 6211 */
    { ".cdmiq",   "application/cdmi-queue"},               /* RFC 6212 */
    { ".cer",     "application/pkix-cert" },
    { ".cha",     "application/x-chat" },
    { ".chat",    "application/x-chat" },
    { ".class",   "application/octet-stream" },
    { ".clp",     "application/x-msclip" },
    { ".com",     "application/octet-stream" },
    { ".conf",    "text/plain" },
    { ".cpio",    "application/x-cpio" },
    { ".cpp",     "text/plain" },
    { ".cpt",     "application/x-cpt" },
    { ".crl",     "application/pkix-crl" },
    { ".crt",     "application/x-x509-ca-cert" },
    { ".csh",     "application/x-csh" },
    { ".css",     "text/css" },
    { ".csv",     "text/csv" },
    { ".cxx",     "text/plain" },
    { ".davmount","application/davmount+xml"},            /* RFC 4918 */
    { ".dcr",     "application/x-director" },
    { ".deepv",   "application/x-deepv" },
    { ".def",     "text/plain" },
    { ".der",     "application/x-x509-ca-cert" },
    { ".dia",     "application/x-dia" },
    { ".dif",     "video/x-dv" },
    { ".dir",     "application/x-director" },
    { ".dl",      "video/x-dl" },
    { ".dll",     "application/x-msdownload" },
    { ".dms",     "application/octet-stream" },
    { ".doc",     "application/msword" },
    { ".dot",     "application/msword" },
    { ".dp",      "application/commonground" },
    { ".drw",     "application/drafting" },
    { ".dsc",     "text/prs.lines.tag"},                   /* IANA: PRS Lines Tag */
    { ".dssc",    "application/dssc+der"},                 /* RFC 5698 */
    { ".dtd",     "application/xml-dtd" },
    { ".dump",    "application/octet-stream" },
    { ".dv",      "video/x-dv" },
    { ".dvi",     "application/x-dvi" },
    { ".dwf",     "model/vnd.dwf" },
    { ".dwg",     "image/vnd.dwg" },
    { ".dxr",     "application/x-director" },
    { ".el",      "text/x-script.elisp" },
    { ".elc",     "application/x-elc" },
    { ".elm",     "text/plain" },
    { ".eml",     "text/plain" },
    { ".env",     "application/x-envoy" },
    { ".eps",     "application/postscript" },
    { ".es",      "application/x-esrehber" },
    { ".etx",     "text/x-setext" },
    { ".evy",     "application/x-envoy" },
    { ".exe",     "application/octet-stream" },
    { ".f",       "text/plain" },
    { ".f77",     "text/x-fortran" },
    { ".f90",     "text/plain" },
    { ".fdf",     "application/vnd.fdf" },
    { ".fif",     "application/fractals" },
    { ".flac",    "audio/flac" },
    { ".fli",     "video/x-fli" },
    { ".flo",     "image/florian" },
    { ".flr",     "x-world/x-vrml" },
    { ".flv",     "video/x-flv" },
    { ".flx",     "text/vnd.fmi.flexstor" },
    { ".fmf",     "video/x-atomic3d-feature" },
    { ".for",     "text/plain" },
    { ".fpx",     "image/vnd.fpx" },
    { ".frl",     "application/freeloader" },
    { ".funk",    "audio/make" },
    { ".g",       "text/plain" },
    { ".g3",      "image/g3fax" },
    { ".gbt",     "text/plain" },
    { ".gif",     "image/gif" },
    { ".gl",      "video/gl" },
    { ".gram",    "application/srgs"},                     /* W3C Speech Grammar */
    { ".grxml",   "application/srgs+xml"},                 /* W3C Speech Grammar */
    { ".gsd",     "audio/x-gsm" },
    { ".gsm",     "audio/x-gsm" },
    { ".gsp",     "application/x-gsp" },
    { ".gss",     "application/x-gss" },
    { ".gtar",    "application/x-gtar" },
    { ".gz",      "application/x-gzip" },
    { ".gzip",    "application/x-gzip" },
    { ".h",       "text/plain" },
    { ".hdf",     "application/x-hdf" },
    { ".help",    "application/x-helpfile" },
    { ".hgl",     "application/vnd.hp-HPGL" },
    { ".hh",      "text/plain" },
    { ".hlb",     "text/x-script" },
    { ".hlp",     "application/winhlp" },
    { ".hpg",     "application/vnd.hp-HPGL" },
    { ".hpgl",    "application/vnd.hp-hpgl" },
    { ".hqx",     "application/mac-binhex40" },
    { ".hta",     "application/hta" },
    { ".htc",     "text/x-component" },
    { ".htt",     "text/webviewhtml" },
    { ".htx",     "text/html" },
    { ".ica",     "application/x-ica" },
    { ".ice",     "x-conference/x-cooltalk" },
    { ".ico",     "image/x-icon" },
    { ".ics",     "text/calendar" },
    { ".idc",     "text/plain" },
    { ".ief",     "image/ief" },
    { ".iefs",    "image/ief" },
    { ".ifb",     "text/calendar" },
    { ".iges",    "application/iges" },
    { ".igs",     "application/iges" },
    { ".iii",     "application/x-iphone" },
    { ".ima",     "application/x-ima" },
    { ".imap",    "application/x-httpd-imap" },
    { ".inf",     "application/inf" },
    { ".ins",     "application/x-internet-signup" },
    { ".ip",      "application/x-ip2" },
    { ".ipfix",   "application/ipfix"},                    /* RFC 3917 */
    { ".isp",     "application/x-internet-signup" },
    { ".isu",     "video/x-isvideo" },
    { ".it",      "audio/it" },
    { ".iv",      "application/x-inventor" },
    { ".ivr",     "i-world/i-vrml" },
    { ".ivy",     "application/x-livescreen" },
    { ".jam",     "audio/x-jam" },
    { ".jar",     "application/x-java-archive" },
    { ".jav",     "text/plain" },
    { ".java",    "text/plain" },
    { ".jcm",     "application/x-java-commerce" },
    { ".jfif",    "image/jpeg" },
    { ".jfif-tbnl", "image/jpeg" },
    { ".jng",     "image/x-jng" },
    { ".jpe",     "image/jpeg" },
    { ".jpeg",    "image/jpeg" },
    { ".jpg",     "image/jpeg" },
    { ".jpgv",    "video/jpeg"},                           /* RFC 3555 */
    { ".jpm",     "video/jpm"},                            /* IANA: JPM */
    { ".jps",     "image/x-jps" },
    { ".js",      "application/x-javascript" },
    { ".jut",     "image/jutvision" },
    { ".kar",     "audio/midi" },
    { ".ksh",     "application/x-ksh" },
    { ".la",      "audio/nspaudio" },
    { ".lam",     "audio/x-liveaudio" },
    { ".latex",   "application/x-latex" },
    { ".lha",     "application/octet-stream" },
    { ".lhx",     "application/octet-stream" },
    { ".list",    "text/plain" },
    { ".lma",     "audio/nspaudio" },
    { ".log",     "text/plain" },
    { ".ls",      "application/x-javascript" },
    { ".lsf",     "video/x-la-asf" },
    { ".lsp",     "application/x-lisp" },
    { ".lst",     "text/plain" },
    { ".lsx",     "video/x-la-asf" },
    { ".ltx",     "application/x-latex" },
    { ".lzh",     "application/x-lzh" },
    { ".lzx",     "application/lzx" },
    { ".m",       "text/plain" },
    { ".m13",     "application/x-msmediaview" },
    { ".m14",     "application/x-msmediaview" },
    { ".m1v",     "video/mpeg" },
    { ".m2a",     "audio/mpeg" },
    { ".m2v",     "video/mpeg" },
    { ".m3u",     "audio/x-mpegurl" },
    { ".m4a",     "audio/mp4" },
    { ".m4p",     "audio/mp4" },
    { ".m4v",     "video/x-m4v"},                          /* Wikipedia: M4v */
    { ".ma",      "application/mathematica"},              /* IANA - Mathematica */
    { ".mads",    "application/mads+xml"},                 /* RFC 6207 */
    { ".man",     "application/x-troff-man" },
    { ".map",     "application/x-navimap" },
    { ".mar",     "text/plain" },
    { ".mathml",  "application/mathml+xml"},               /* W3C Math Home */
    { ".mbd",     "application/mbedlet" },
    { ".mbox",    "application/mbox"},                     /* RFC 4155 */
    { ".mc$",     "application/x-magic-cap-package-1.0" },
    { ".mcd",     "application/mcad" },
    { ".mcf",     "text/mcf" },
    { ".mcp",     "application/netmc" },
    { ".mdb",     "application/x-msaccess" },
    { ".me",      "application/x-troff-me" },
    { ".mets",    "application/mets+xml"},                 /* RFC 6207 */
    { ".mht",     "message/rfc822" },
    { ".mhtml",   "message/rfc822" },
    { ".mid",     "audio/midi" },
    { ".midi",    "audio/x-midi" },
    { ".mif",     "application/vnd.mif" },
    { ".mime",    "message/rfc822" },
    { ".mj2",     "video/mj2"},                            /* IANA: MJ2 */
    { ".mjf",     "audio/x-vnd.AudioExplosion.MjuiceMediaFile" },
    { ".mjpg",    "video/x-motion-jpeg" },
    { ".mm",      "application/base64" },
    { ".mme",     "application/base64" },
    { ".mng",     "image/x-mng" },
    { ".mny",     "application/x-msmoney" },
    { ".mocha",   "application/x-javascript" },
    { ".mod",     "audio/mod" },
    { ".mods",    "application/mods+xml"},                 /* RFC 6207 */
    { ".moov",    "video/quicktime" },
    { ".mov",     "video/quicktime" },
    { ".movie",   "video/x-sgi-movie" },
    { ".mp2",     "audio/mpeg" },
    { ".mp3",     "audio/mpeg" },
    { ".mp4",     "video/mp4" },
    { ".mp4a",    "audio/mp4" },
    { ".mpa",     "video/mpeg" },
    { ".mpc",     "application/x-project" },
    { ".mpe",     "video/mpeg" },
    { ".mpeg",    "video/mpeg" },
    { ".mpg",     "video/mpeg" },
    { ".mpga",    "audio/mpeg" },
    { ".mpp",     "application/vnd.ms-project" },
    { ".mpt",     "application/x-project" },
    { ".mpv",     "application/x-project" },
    { ".mpv2",    "video/mpeg" },
    { ".mpx",     "application/x-project" },
    { ".mrc",     "application/marc" },
    { ".mrcx",    "application/marcxml+xml"},              /* RFC 6207 */
    { ".ms",      "application/x-troff-ms" },
    { ".mscml",   "application/mediaservercontrol+xml"},   /* RFC 5022 */
    { ".msh",     "model/mesh"},                           /* RFC 2077 */
    { ".mv",      "video/x-sgi-movie" },
    { ".mvb",     "application/x-msmediaview" },
    { ".mxf",     "application/mxf"},                      /* RFC 4539 */
    { ".mxu",     "video/vnd.mpegurl" },
    { ".my",      "audio/make" },
    { ".mzz",     "application/x-vnd.AudioExplosion.mzz" },
    { ".nap",     "image/naplps" },
    { ".naplps",  "image/naplps" },
    { ".nc",      "application/x-netcdf" },
    { ".ncm",     "applicaction/vnd.nokia.configuration-message" },
    { ".nif",     "image/x-niff" },
    { ".niff",    "image/x-niff" },
    { ".nix",     "application/x-mix-transfer" },
    { ".nsc",     "application/x-conference" },
    { ".nvd",     "application/x-navidoc" },
    { ".nvm",     "application/x-navimap" },
    { ".nws",     "message/rfc822" },
    { ".o",       "application/octet-stream" },
    { ".oda",     "application/oda" },
    { ".odc",     "application/vnd.oasis.opendocument.chart" },
    { ".odf",     "application/vnd.oasis.opendocument.formula" },
    { ".odg",     "application/vnd.oasis.opendocument.graphics" },
    { ".odi",     "application/vnd.oasis.opendocument.image" },
    { ".odm",     "application/vnd.oasis.opendocument.text-master" },
    { ".odp",     "application/vnd.oasis.opendocument.presentation" },
    { ".ods",     "application/vnd.oasis.opendocument.spreadsheet" },
    { ".odt",     "application/vnd.oasis.opendocument.text" },
    { ".oga",     "audio/ogg" },
    { ".ogg",     "audio/ogg" },
    { ".ogv",     "video/ogg" },
    { ".ogx",     "application/ogg" },
    { ".omc",     "application/x-omc" },
    { ".omcd",    "application/x-omcdatamaker" },
    { ".omcr",    "application/x-omcregerator" },
    { ".p10",     "application/pkcs10" },
    { ".p12",     "application/x-pkcs-12" },
    { ".p7a",     "application/x-pkcs7-signature" },
    { ".p7b",     "application/x-pkcs7-certificates" },
    { ".p7c",     "application/x-pkcs7-mime" },
    { ".p7m",     "application/x-pkcs7-mime" },
    { ".p7r",     "application/x-pkcs7-certreqresp" },
    { ".p7s",     "application/x-pkcs7-signature" },
    { ".p8",      "application/pkcs8"},                    /* RFC 5208 */
    { ".pac",     "application/x-ns-proxy-autoconfig" },
    { ".part",    "application/pro_eng" },
    { ".pbm",     "image/x-portable-bitmap" },
    { ".pcl",     "application/vnd.hp-pcl" },
    { ".pct",     "image/x-pict" },
    { ".pcx",     "image/x-pcx" },
    { ".pdb",     "chemical/x-pdb" },
    { ".pdf",     "application/pdf" },
    { ".pfr",     "application/font-tdpfr"},               /* RFC 3073 */
    { ".pfunk",   "audio/make" },
    { ".pfx",     "application/x-pkcs12" },
    { ".pgm",     "image/x-portable-graymap" },
    { ".pgp",     "application/pgp-signature"},            /* RFC 2015 */
    { ".pic",     "image/pict" },
    { ".pict",    "image/pict" },
    { ".pkg",     "application/x-newton-compatible-pkg" },
    { ".pki",     "application/pkixcmp"},                  /* RFC 2585 */
    { ".pkipath", "application/pkix-pkipath"},             /* RFC 2585 */
    { ".pko",     "application/vnd.ms-pki.pko" },
    { ".pl",      "text/plain" },
    { ".pls",     "application/pls+xml"},                  /* RFC 4267 */
    { ".plx",     "application/x-PiXCLscript" },
    { ".pm",      "image/x-xpixmap" },
    { ".pm4",     "application/x-pagemaker" },
    { ".pm5",     "application/x-pagemaker" },
    { ".pma",     "application/x-perfmon" },
    { ".pmc",     "application/x-perfmon" },
    { ".pml",     "application/x-perfmon" },
    { ".pmr",     "application/x-perfmon" },
    { ".pmw",     "application/x-perfmon" },
    { ".png",     "image/png" },
    { ".pnm",     "image/x-portable-anymap" },
    { ".pot",     "application/vnd.ms-powerpoint" },
    { ".pov",     "model/x-pov" },
    { ".ppa",     "application/vnd.ms-powerpoint" },
    { ".ppm",     "image/x-portable-pixmap" },
    { ".pps",     "application/vnd.ms-powerpoint" },
    { ".ppt",     "application/vnd.ms-powerpoint" },
    { ".ppz",     "application/vnd.ms-powerpoint" },
    { ".pre",     "application/vnd.lotus-freelance" },
    { ".prf",     "application/pics-rules" },
    { ".prt",     "application/pro_eng" },
    { ".ps",      "application/postscript" },
    { ".psd",     "image/vnd.adobe.photoshop" },
    { ".pskcxml", "application/pskc+xml"},                 /* RFC 6030 */
    { ".pub",     "application/x-mspublisher" },
    { ".pvu",     "paleovu/x-pv" },
    { ".pwz",     "application/vnd.ms-powerpoint" },
    { ".py",      "text/x-script.python" },
    { ".pyc",     "application/x-bytecode.python" },
    { ".qcp",     "audio/vnd.qcelp" },
    { ".qd3",     "x-world/x-3dmf" },
    { ".qd3d",    "x-world/x-3dmf" },
    { ".qif",     "image/x-quicktime" },
    { ".qt",      "video/quicktime" },
    { ".qtc",     "video/x-qtc" },
    { ".qti",     "image/x-quicktime" },
    { ".qtif",    "image/quicktime" },
    { ".ra",      "audio/x-pn-realaudio" },
    { ".ram",     "audio/x-pn-realaudio" },
    { ".rar",     "application/x-rar-compressed" },
    { ".ras",     "image/x-cmu-raster" },
    { ".rast",    "image/x-cmu-raster" },
    { ".rdf",     "application/rdf+xml" },
    { ".rexx",    "text/x-script.rexx" },
    { ".rf",      "image/vnd.rn-realflash" },
    { ".rgb",     "image/x-rgb" },
    { ".rl",      "application/resource-lists+xml"},       /* RFC 4826 */
    { ".rld",     "application/resource-lists-diff+xml"},  /* RFC 4826 */
    { ".rm",      "audio/x-pn-realaudio" },
    { ".rmi",     "audio/midi" },
    { ".rmm",     "audio/x-pn-realaudio" },
    { ".rmp",     "audio/x-pn-realaudio-plugin" },
    { ".rng",     "application/vnd.nokia.ringing-tone" },
    { ".rnx",     "application/vnd.rn-realplayer" },
    { ".roff",    "application/x-roff" },
    { ".rp",      "image/vnd.rn-realpix" },
    { ".rpm",     "application/octet-stream" },
    { ".rq",      "application/sparql-query"},             /* W3C SPARQL */
    { ".rs",      "application/rls-services+xml"},         /* RFC 4826 */
    { ".rss",     "application/rss+xml" },
    { ".rt",      "text/richtext" },
    { ".rtf",     "text/rtf" },
    { ".rtx",     "text/richtext" },
    { ".rv",      "video/vnd.rn-realvideo" },
    { ".s",       "text/plain" },
    { ".s3m",     "audio/s3m" },
    { ".saveme",  "application/octet-stream" },
    { ".sbk",     "application/x-tbook" },
    { ".sbml",    "application/sbml+xml"},                 /* RFC 3823 */
    { ".scd",     "application/x-msschedule" },
    { ".scm",     "video/x-scm" },
    { ".scq",     "application/scvp-cv-request"},          /* RFC 5055 */
    { ".scs",     "application/scvp-cv-response"},         /* RFC 5055 */
    { ".sct",     "text/scriptlet" },
    { ".sdml",    "text/plain" },
    { ".sdp",     "application/sdp" },
    { ".sdr",     "application/sounder" },
    { ".sea",     "application/sea" },
    { ".set",     "application/set" },
    { ".setpay",  "application/set-payment-initiation" },
    { ".setreg",  "application/set-registration-initiation" },
    { ".sgm",     "text/sgml" },
    { ".sgml",    "text/sgml" },
    { ".sh",      "application/x-sh" },
    { ".shar",    "application/x-shar" },
    { ".shf",     "application/shf+xml"},                  /* RFC 4194 */
    { ".sid",     "audio/x-psid" },
    { ".sit",     "application/x-stuffit" },
    { ".skd",     "application/x-koan" },
    { ".skm",     "application/x-koan" },
    { ".skp",     "application/x-koan" },
    { ".skt",     "application/x-koan" },
    { ".sl",      "application/x-seelogo" },
    { ".smi",     "application/smil+xml" },
    { ".smil",    "application/smil" },
    { ".snd",     "audio/basic" },
    { ".sol",     "application/solids" },
    { ".spc",     "application/x-pkcs7-certificate" },
    { ".spl",     "application/futuresplash" },
    { ".spp",     "application/scvp-vp-response"},         /* RFC 5055 */
    { ".spq",     "application/scvp-vp-request"},          /* RFC 5055 */
    { ".spr",     "application/x-sprite" },
    { ".sprite",  "application/x-sprite" },
    { ".spx",     "audio/ogg" },
    { ".sql",     "application/x-sql" },
    { ".src",     "application/x-wais-source" },
    { ".sru",     "application/sru+xml"},                  /* RFC 6207 */
    { ".srx",     "application/sparql-results+xml"},       /* W3C SPARQL */
    { ".ssm",     "application/streamingmedia" },
    { ".ssml",    "application/ssml+xml"},                 /* W3C Speech Synthesis */
    { ".sst",     "application/vnd.ms-pki.certstore" },
    { ".stc",     "application/vnd.sun.xml.calc.template" },
    { ".std",     "application/vnd.sun.xml.draw.template" },
    { ".step",    "application/step" },
    { ".sti",     "application/vnd.sun.xml.impress.template" },
    { ".stk",     "application/hyperstudio"},              /* IANA - Hyperstudio */
    { ".stl",     "application/x-navistyle" },
    { ".stm",     "text/html" },
    { ".stp",     "application/step" },
    { ".stw",     "application/vnd.sun.xml.writer.template" },
    { ".sv4cpio", "application/x-sv4cpio" },
    { ".sv4crc",  "application/x-sv4crc" },
    { ".svf",     "image/vnd.dwg" },
    { ".svg",     "image/svg+xml" },
    { ".svgz",    "image/x-svgz" },
    { ".svr",     "application/x-world" },
    { ".swf",     "application/x-shockwave-flash" },
    { ".sxc",     "application/vnd.sun.xml.calc" },
    { ".sxd",     "application/vnd.sun.xml.draw" },
    { ".sxg",     "application/vnd.sun.xml.writer.global" },
    { ".sxi",     "application/vnd.sun.xml.impress" },
    { ".sxm",     "application/vnd.sun.xml.math" },
    { ".sxw",     "application/vnd.sun.xml.writer" },
    { ".t",       "application/x-troff" },
    { ".talk",    "text/x-speech" },
    { ".tar",     "application/x-tar" },
    { ".tbk",     "application/toolbook" },
    { ".tcl",     "text/plain" },
    { ".tcsh",    "text/x-script.tcsh" },
    { ".tei",     "application/tei+xml"},                  /* RFC 6129 */
    { ".tex",     "application/x-tex" },
    { ".texi",    "application/x-texinfo" },
    { ".texinfo", "application/x-texinfo" },
    { ".text",    "text/plain" },
    { ".tfi",     "application/thraud+xml"},               /* RFC 5941 */
    { ".tgz",     "application/x-gzip" },
    { ".tif",     "image/tiff" },
    { ".tiff",    "image/tiff" },
    { ".torrent", "application/x-bittorrent" },
    { ".tr",      "application/x-troff" },
    { ".tsd",     "application/timestamped-data"},         /* RFC 5955 */
    { ".tsi",     "audio/tsp-audio" },
    { ".tsp",     "audio/tsplayer" },
    { ".tsv",     "text/tab-separated-values" },
    { ".turbot",  "image/florian" },
    { ".txt",     "text/plain" },
    { ".uil",     "text/x-uil" },
    { ".uls",     "text/iuls" },
    { ".uni",     "text/uri-list" },
    { ".unis",    "text/uri-list" },
    { ".unv",     "application/i-deas" },
    { ".uri",     "text/uri-list" },
    { ".uris",    "text/uri-list" },
    { ".ustar",   "application/x-ustar" },
    { ".uu",      "text/x-uuencode" },
    { ".uue",     "text/x-uuencode" },
    { ".vcd",     "application/x-cdlink" },
    { ".vcf",     "text/x-vcard" },
    { ".vcs",     "text/x-vcalendar" },
    { ".vda",     "application/vda" },
    { ".vdo",     "video/vdo" },
    { ".vew",     "application/groupwise" },
    { ".viv",     "video/vnd.vivo" },
    { ".vivo",    "video/vnd.vivo" },
    { ".vmd",     "application/vocaltec-media-desc" },
    { ".vmf",     "application/vocaltect-media-file" },
    { ".voc",     "audio/voc" },
    { ".vos",     "video/vosaic" },
    { ".vox",     "audio/voxware" },
    { ".vqe",     "audio/x-twinvq-plugin" },
    { ".vqf",     "audio/x-twinvq" },
    { ".vql",     "audio/x-twinvq-plugin" },
    { ".vrml",    "x-world/x-vrml" },
    { ".vrt",     "x-world/x-vrt" },
    { ".vsd",     "application/vnd.visio" },
    { ".vst",     "application/x-visio" },
    { ".vsw",     "application/x-visio" },
    { ".vxml",    "application/voicexml+xml"},             /* RFC 4267 */
    { ".w60",     "application/wordperfect6.0" },
    { ".w61",     "application/wordperfect6.1" },
    { ".w6w",     "application/msword" },
    { ".wav",     "audio/x-wav" },
    { ".wb1",     "application/x-qpro" },
    { ".wbmp",    "image/vnd.wap.wbmp" },
    { ".wcm",     "application/vnd.ms-works" },
    { ".wdb",     "application/vnd.ms-works" },
    { ".web",     "application/vnd.xara" },
    { ".weba",    "audio/webm"},                           /* WebM Project */
    { ".webm",    "video/webm"},                           /* WebM Project */
    { ".webp",    "image/webp"},                           /* Wikipedia: WebP */
    { ".wgt",     "application/widget"},                   /* W3C Widget Packaging and XML Configuration */
    { ".wiz",     "application/msword" },
    { ".wk1",     "application/x-123" },
    { ".wks",     "application/vnd.ms-works" },
    { ".wma",     "audio/x-ms-wma" },
    { ".wmf",     "application/x-msmetafile" },
    { ".wml",     "text/vnd.wap.wml" },
    { ".wmlc",    "application/vnd.wap.wmlc" },
    { ".wmls",    "text/vnd.wap.wmlscript" },
    { ".wmlsc",   "application/vnd.wap.wmlscript" },
    { ".woff",    "application/x-font-woff"},              /* Wikipedia: Web Open Font Format */
    { ".word",    "application/msword" },
    { ".wp",      "application/wordperfect" },
    { ".wp5",     "application/wordperfect" },
    { ".wp6",     "application/wordperfect" },
    { ".wpd",     "application/wordperfect" },
    { ".wps",     "application/vnd.ms-works" },
    { ".wq1",     "application/x-lotus" },
    { ".wri",     "application/x-mswrite" },
    { ".wrl",     "x-world/x-vrml" },
    { ".wrz",     "x-world/x-vrml" },
    { ".wsc",     "text/scriplet" },
    { ".wsdl",    "application/wsdl+xml"},                 /* W3C Web Service Description Language */
    { ".wspolicy","application/wspolicy+xml"},            /* W3C Web Services Policy */
    { ".wsrc",    "application/x-wais-source" },
    { ".wtk",     "application/x-wintalk" },
    { ".x-png",   "image/png" },
    { ".xaf",     "x-world/x-vrml" },
    { ".xbm",     "image/x-xbitmap" },
    { ".xdr",     "video/x-amt-demorun" },
    { ".xdssc",   "application/dssc+xml"},                 /* RFC 5698 */
    { ".xenc",    "application/xenc+xml"},                 /* W3C XML Encryption Syntax and Processing */
    { ".xer",     "application/patch-ops-error+xml"},      /* RFC 5261 */
    { ".xgz",     "xgl/drawing" },
    { ".xht",     "application/xhtml+xml" },
    { ".xhtml",   "application/xhtml+xml" },
    { ".xif",     "image/vnd.xiff" },
    { ".xl",      "application/excel" },
    { ".xla",     "application/vnd.ms-excel" },
    { ".xlb",     "application/vnd.ms-excel" },
    { ".xlc",     "application/vnd.ms-excel" },
    { ".xld",     "application/vnd.ms-excel" },
    { ".xlk",     "application/vnd.ms-excel" },
    { ".xll",     "application/vnd.ms-excel" },
    { ".xlm",     "application/vnd.ms-excel" },
    { ".xls",     "application/vnd.ms-excel" },
    { ".xlt",     "application/vnd.ms-excel" },
    { ".xlv",     "application/vnd.ms-excel" },
    { ".xlw",     "application/vnd.ms-excel" },
    { ".xm",      "audio/xm" },
    { ".xml",     "application/xml" },
    { ".xmz",     "xgl/movie" },
    { ".xof",     "x-world/x-vrml" },
    { ".xop",     "application/xop+xml"},                  /* W3C XOP */
    { ".xpix",    "application/x-vnd.ls-xpix" },
    { ".xpm",     "image/x-xpixmap" },
    { ".xsl",     "application/xml" },
    { ".xslt",    "application/xslt+xml" },
    { ".xspf",    "application/xspf+xml" },
    { ".xsr",     "video/x-amt-showrun" },
    { ".xul",     "application/vnd.mozilla.xul+xml" },
    { ".xwd",     "image/x-xwindowdump" },
    { ".xyz",     "chemical/x-xyz" },
    { ".z",       "application/x-compressed" },
    { ".zip",     "application/zip" },
    { ".zoo",     "application/octet-stream" },
    { ".zsh",     "text/x-script.zsh" },

    { NULL,     NULL }
};


/*
 *----------------------------------------------------------------------
 *
 * NsConfigMimeTypes --
 *
 *      Add compiled-in and configured mime types.
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
NsConfigMimeTypes(void)
{
    Ns_Set     *set;
    size_t      i;
    static int  once = 0;

    if (once == 0) {
        once = 1;

        /*
         * Initialize hash table of file extensions.
         */

        Tcl_InitHashTable(&types, TCL_STRING_KEYS);

        /*
         * Add default system types first from above
         */

        for (i = 0U; typetab[i].ext != NULL; ++i) {
            AddType(typetab[i].ext, typetab[i].type);
        }
    }

    set = Ns_ConfigGetSection("ns/mimetypes");
    if (set == NULL) {
        return;
    }

    defaultType = Ns_SetIGet(set, "default");
    if (defaultType == NULL) {
        defaultType = TYPE_DEFAULT;
    }

    noextType = Ns_SetIGet(set, "noextension");
    if (noextType == NULL) {
        noextType = defaultType;
    }

    for (i = 0U; i < Ns_SetSize(set); i++) {
        AddType(Ns_SetKey(set, i), Ns_SetValue(set, i));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetMimeType --
 *
 *      Guess the mime type based on filename extension. Case is
 *      ignored.
 *
 * Results:
 *      A mime type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_GetMimeType(const char *file)
{
    const char    *start, *ext;
    Ns_DString     ds;
    Tcl_HashEntry *hPtr;

    assert(file != NULL);
    
    start = strrchr(file, '/');
    if (start == NULL) {
        start = file;
    }
    ext = strrchr(start, '.');
    if (ext == NULL) {
        return noextType;
    }
    Ns_DStringInit(&ds);
    ext = LowerDString(&ds, ext);
    hPtr = Tcl_FindHashEntry(&types, ext);
    Ns_DStringFree(&ds);
    if (hPtr != NULL) {
        return Tcl_GetHashValue(hPtr);
    }

    return defaultType;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGuessTypeObjCmd --
 *
 *      Implements ns_guesstype.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclGuessTypeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *type;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "filename");
        return TCL_ERROR;
    }
    type = Ns_GetMimeType(Tcl_GetString(objv[1]));
    Tcl_SetObjResult(interp, Tcl_NewStringObj(type, -1));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetMimeTypes --
 *
 *      Append list of configured extension / mime-type mappings to
 *      given dstring.
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
NsGetMimeTypes(Ns_DString *dsPtr)
{
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

    hPtr = Tcl_FirstHashEntry(&types, &search);
    while (hPtr != NULL) {
        Tcl_DStringAppendElement(dsPtr, Tcl_GetHashKey(&types, hPtr));
        Tcl_DStringAppendElement(dsPtr, Tcl_GetHashValue(hPtr));
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AddType --
 *
 *      Add a mime type to the global hash table.
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
AddType(CONST char *ext, CONST char *type)
{
    Ns_DString      ds;
    Tcl_HashEntry  *he;
    int             isNew;

    Ns_DStringInit(&ds);
    ext = LowerDString(&ds, ext);
    he = Tcl_CreateHashEntry(&types, ext, &isNew);
    if (isNew == 0) {
	char *oldType = Tcl_GetHashValue(he);

	if (STREQ(oldType, type)) {
	    Ns_Log(Warning, 
		   "config mimtypes: redefine mime type for %s with identical value (%s); statement useless",
		   ext, oldType);
	} else {
	    Ns_Log(Warning, 
		   "config mimtypes: redefine predefined mime type for %s value '%s' with different value: %s",
		   ext, oldType, type);
	}

        ns_free(oldType);
    }
    Tcl_SetHashValue(he, ns_strdup(type));
    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * LowerDString --
 *
 *      Append a string to the dstring, converting all alphabetic
 *      characeters to lowercase.
 *
 * Results:
 *      dsPtr->string
 *
 * Side effects:
 *      Appends to dstring.
 *
 *----------------------------------------------------------------------
 */

static char *
LowerDString(Ns_DString *dsPtr, CONST char *ext)
{
    char *p;

    Ns_DStringAppend(dsPtr, ext);
    p = dsPtr->string;
    while (*p != '\0') {
        if (CHARTYPE(upper, *p) != 0) {
            *p = CHARCONV(lower, *p);
        }
        ++p;
    }

    return dsPtr->string;
}
