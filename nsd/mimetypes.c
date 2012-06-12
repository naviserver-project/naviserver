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

static struct exttype {
    CONST char     *ext;
    CONST char     *type;
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

    { ".323",   "text/h323" },
    { ".3dm",   "x-world/x-3dmf" },
    { ".3dmf",  "x-world/x-3dmf" },
    { ".aab",   "application/x-authorware-bin" },
    { ".aam",   "application/x-authorware-map" },
    { ".aas",   "application/x-authorware-seg" },
    { ".abc",   "text/vnd.abc" },
    { ".acgi",  "text/html" },
    { ".acx",   "application/internet-property-stream" },
    { ".afl",   "video/animaflex" },
    { ".ai",    "application/postscript" },
    { ".aif",   "audio/aiff" },
    { ".aifc",  "audio/aiff" },
    { ".aiff",  "audio/aiff" },
    { ".aim",   "application/x-aim" },
    { ".aip",   "text/x-audiosoft-intra" },
    { ".ani",   "application/x-navi-animation" },
    { ".anx",   "application/annodex" },
    { ".aos",   "application/x-nokia-9000-communicator-add-on-software" },
    { ".aps",   "application/mime" },
    { ".arc",   "application/octet-stream" },
    { ".arj",   "application/arj" },
    { ".art",   "image/x-art" },
    { ".asf",   "video/x-ms-asf" },
    { ".asm",   "text/x-asm" },
    { ".asp",   "text/asp" },
    { ".asr",   "video/x-ms-asf" },
    { ".asx",   "video/x-ms-asf" },
    { ".atom",  "application/atom+xml" },
    { ".au",    "audio/basic" },
    { ".avi",   "video/x-msvideo" },
    { ".avs",   "video/avs-video" },
    { ".axa",   "audio/annodex" },
    { ".axv",   "video/annodex" },
    { ".axs",   "application/olescript" },
    { ".bas",   "text/plain" },
    { ".bin",   "application/x-macbinary" },
    { ".bcpio", "application/x-bcpio" },
    { ".bm",    "image/bmp" },
    { ".bmp",   "image/bmp" },
    { ".boo",   "application/book" },
    { ".book",  "application/book" },
    { ".boz",   "application/x-bzip2" },
    { ".bsh",   "application/x-bsh" },
    { ".bz",    "application/x-bzip" },
    { ".bz2",   "application/x-bzip2" },
    { ".c",     "text/plain" },
    { ".c++",   "text/plain" },
    { ".cat",   "application/vnd.ms-pki.secret" },
    { ".cc",    "text/plain" },
    { ".ccad",  "application/clariscad" },
    { ".cco",   "application/x-cocoa" },
    { ".cdf",   "application/x-cdf" },
    { ".cer",   "application/x-x509-ca-cert" },
    { ".cha",   "application/x-chat" },
    { ".chat",  "application/x-chat" },
    { ".class", "application/octet-stream" },
    { ".clp",   "application/x-msclip" },
    { ".com",   "application/octet-stream" },
    { ".conf",  "text/plain" },
    { ".cpio",  "application/x-cpio" },
    { ".cpp",   "text/plain" },
    { ".cpt",   "application/x-cpt" },
    { ".crl",   "application/pkix-crl" },
    { ".crt",   "application/x-x509-ca-cert" },
    { ".csh",   "application/x-csh" },
    { ".css",   "text/css" },
    { ".csv",   "text/csv" },
    { ".cxx",   "text/plain" },
    { ".dcr",   "application/x-director" },
    { ".deepv", "application/x-deepv" },
    { ".def",   "text/plain" },
    { ".der",   "application/x-x509-ca-cert" },
    { ".dia",   "application/x-dia" },
    { ".dif",   "video/x-dv" },
    { ".dir",   "application/x-director" },
    { ".dl",    "video/x-dl" },
    { ".dll",   "application/x-msdownload" },
    { ".dms",   "application/octet-stream" },
    { ".doc",   "application/msword" },
    { ".dot",   "application/msword" },
    { ".dp",    "application/commonground" },
    { ".drw",   "application/drafting" },
    { ".dtd",   "application/xml-dtd" },
    { ".dump",  "application/octet-stream" },
    { ".dv",    "video/x-dv" },
    { ".dvi",   "application/x-dvi" },
    { ".dwf",   "model/vnd.dwf" },
    { ".dwg",   "image/vnd.dwg" },
    { ".dxr",   "application/x-director" },
    { ".el",    "text/x-script.elisp" },
    { ".elc",   "application/x-elc" },
    { ".elm",   "text/plain" },
    { ".eml",   "text/plain" },
    { ".env",   "application/x-envoy" },
    { ".eps",   "application/postscript" },
    { ".es",    "application/x-esrehber" },
    { ".etx",   "text/x-setext" },
    { ".evy",   "application/x-envoy" },
    { ".exe",   "application/octet-stream" },
    { ".fif",   "application/fractals" },
    { ".flr",   "x-world/x-vrml" },
    { ".f",     "text/plain" },
    { ".f77",   "text/x-fortran" },
    { ".f90",   "text/plain" },
    { ".fdf",   "application/vnd.fdf" },
    { ".flac",  "audio/flac" },
    { ".fli",   "video/x-fli" },
    { ".flo",   "image/florian" },
    { ".flv",   "video/x-flv" },
    { ".flx",   "text/vnd.fmi.flexstor" },
    { ".fmf",   "video/x-atomic3d-feature" },
    { ".for",   "text/plain" },
    { ".fpx",   "image/vnd.fpx" },
    { ".frl",   "application/freeloader" },
    { ".funk",  "audio/make" },
    { ".g",     "text/plain" },
    { ".g3",    "image/g3fax" },
    { ".gbt",   "text/plain" },
    { ".gif",   "image/gif" },
    { ".gl",    "video/gl" },
    { ".gsd",   "audio/x-gsm" },
    { ".gsm",   "audio/x-gsm" },
    { ".gsp",   "application/x-gsp" },
    { ".gss",   "application/x-gss" },
    { ".gtar",  "application/x-gtar" },
    { ".gz",    "application/x-compressed" },
    { ".gzip",  "application/x-gzip" },
    { ".h",     "text/plain" },
    { ".hdf",   "application/x-hdf" },
    { ".help",  "application/x-helpfile" },
    { ".hgl",   "application/vnd.hp-HPGL" },
    { ".hh",    "text/plain" },
    { ".hlb",   "text/x-script" },
    { ".hlp",   "application/winhlp" },
    { ".hpg",   "application/vnd.hp-HPGL" },
    { ".hpgl",  "application/vnd.hp-HPGL" },
    { ".hqx",   "application/mac-binhex40" },
    { ".hta",   "application/hta" },
    { ".htc",   "text/x-component" },
    { ".htt",   "text/webviewhtml" },
    { ".htx",   "text/html" },
    { ".ico",   "image/x-icon" },
    { ".ica",   "application/x-ica" },
    { ".ice",   "x-conference/x-cooltalk" },
    { ".ico",   "image/x-icon" },
    { ".ics",   "text/calendar" },
    { ".idc",   "text/plain" },
    { ".ief",   "image/ief" },
    { ".iefs",  "image/ief" },
    { ".ifb",   "text/calendar" },
    { ".iges",  "application/iges" },
    { ".igs",   "application/iges" },
    { ".iii",   "application/x-iphone" },
    { ".ima",   "application/x-ima" },
    { ".imap",  "application/x-httpd-imap" },
    { ".inf",   "application/inf" },
    { ".ins",   "application/x-internet-signup" },
    { ".ip",    "application/x-ip2" },
    { ".isp",   "application/x-internet-signup" },
    { ".isu",   "video/x-isvideo" },
    { ".it",    "audio/it" },
    { ".iv",    "application/x-inventor" },
    { ".ivr",   "i-world/i-vrml" },
    { ".ivy",   "application/x-livescreen" },
    { ".jam",   "audio/x-jam" },
    { ".jav",   "text/plain" },
    { ".java",  "text/plain" },
    { ".jar",   "application/x-java-archive" },
    { ".jcm",   "application/x-java-commerce" },
    { ".jfif",  "image/jpeg" },
    { ".jfif-tbnl", "image/jpeg" },
    { ".jng",   "image/x-jng" },
    { ".jpe",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".jpg",   "image/jpeg" },
    { ".jps",   "image/x-jps" },
    { ".js",    "application/x-javascript" },
    { ".jut",   "image/jutvision" },
    { ".kar",   "audio/midi" },
    { ".ksh",   "application/x-ksh" },
    { ".la",    "audio/nspaudio" },
    { ".latex", "application/x-latex" },
    { ".lam",   "audio/x-liveaudio" },
    { ".lha",   "application/octet-stream" },
    { ".lhx",   "application/octet-stream" },
    { ".list",  "text/plain" },
    { ".lma",   "audio/nspaudio" },
    { ".log",   "text/plain" },
    { ".ls",    "application/x-javascript" },
    { ".lsf",   "video/x-la-asf" },
    { ".lsp",   "application/x-lisp" },
    { ".lst",   "text/plain" },
    { ".lsx",   "video/x-la-asf" },
    { ".ltx",   "application/x-latex" },
    { ".lzh",   "application/x-lzh" },
    { ".lzx",   "application/lzx" },
    { ".m",     "text/plain" },
    { ".m13",   "application/x-msmediaview" },
    { ".m14",   "application/x-msmediaview" },
    { ".m1v",   "video/mpeg" },
    { ".m2a",   "audio/mpeg" },
    { ".m2v",   "video/mpeg" },
    { ".m3u",   "audio/x-mpegurl" },
    { ".m4a",   "audio/mp4" },
    { ".m4p",   "audio/mp4" },
    { ".man",   "application/x-troff-man" },
    { ".map",   "application/x-navimap" },
    { ".mar",   "text/plain" },
    { ".mbd",   "application/mbedlet" },
    { ".mc$",   "application/x-magic-cap-package-1.0" },
    { ".mcd",   "application/mcad" },
    { ".mcf",   "text/mcf" },
    { ".mcp",   "application/netmc" },
    { ".mdb",   "application/x-msaccess" },
    { ".me",    "application/x-troff-me" },
    { ".mht",   "message/rfc822" },
    { ".mhtml", "message/rfc822" },
    { ".mid",   "audio/x-midi" },
    { ".midi",  "audio/x-midi" },
    { ".mif",   "application/x-mif" },
    { ".mime",  "message/rfc822" },
    { ".mjf",   "audio/x-vnd.AudioExplosion.MjuiceMediaFile" },
    { ".mjpg",  "video/x-motion-jpeg" },
    { ".mm",    "appliation/base64" },
    { ".mme",   "application/base64" },
    { ".mng",   "image/x-mng" },
    { ".mny",   "application/x-msmoney" },
    { ".mocha", "application/x-javascript" },
    { ".mod",   "audio/mod" },
    { ".moov",  "video/quicktime" },
    { ".mov",   "video/quicktime" },
    { ".movie", "video/x-sgi-movie" },
    { ".mp2",   "audio/mpeg" },
    { ".mp3",   "audio/mpeg" },
    { ".mp4",   "audio/mp4" },
    { ".mpa",   "video/mpeg" },
    { ".mpc",   "application/x-project" },
    { ".mpe",   "video/mpeg" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".mpga",  "video/mpeg" },
    { ".mpp",   "application/vnd.ms-project" },
    { ".mpt",   "application/x-project" },
    { ".mpv",   "application/x-project" },
    { ".mpv2",  "video/mpeg" },
    { ".mpx",   "application/x-project" },
    { ".mrc",   "application/marc" },
    { ".ms",    "application/x-troff-ms" },
    { ".mv",    "video/x-sgi-movie" },
    { ".my",    "audio/make" },
    { ".mvb",   "application/x-msmediaview" },
    { ".mxu",   "video/vnd.mpegurl" },
    { ".mzz",   "application/x-vnd.AudioExplosion.mzz" },
    { ".nap",   "image/naplps" },
    { ".naplps","image/naplps" },
    { ".nc",    "appliation/x-netcdf" },
    { ".ncm",   "appliation/vnd.nokia.configuration-message" },
    { ".nif",   "image/x-niff" },
    { ".niff",  "image/x-niff" },
    { ".nix",   "application/x-mix-transfer" },
    { ".nsc",   "application/x-conference" },
    { ".nvd",   "application/x-navidoc" },
    { ".nvm",   "application/x-navimap" },
    { ".nws",   "message/rfc822" },
    { ".o",     "application/octet-stream" },
    { ".odt",   "application/vnd.oasis.opendocument.text" },
    { ".ods",   "application/vnd.oasis.opendocument.spreadsheet" },
    { ".odp",   "application/vnd.oasis.opendocument.presentation" },
    { ".odg",   "application/vnd.oasis.opendocument.graphics" },
    { ".odc",   "application/vnd.oasis.opendocument.chart" },
    { ".odf",   "application/vnd.oasis.opendocument.formula" },
    { ".odi",   "application/vnd.oasis.opendocument.image" },
    { ".odm",   "application/vnd.oasis.opendocument.text-master" },
    { ".oda",   "application/oda" },
    { ".oga",   "audio/ogg" },
    { ".ogg",   "audio/ogg" },
    { ".ogv",   "video/ogg" },
    { ".ogx",   "application/ogg" },
    { ".omc",   "application/x-omc" },
    { ".omcd",  "application/x-omcdatamaker" },
    { ".omcr",  "application/x-omcregerator" },
    { ".pac",   "application/x-ns-proxy-autoconfig" },
    { ".p10",   "application/pkcs10" },
    { ".p12",   "application/pkcs-12" },
    { ".p7a",   "application/x-pkcs7-signature" },
    { ".p7b",   "application/x-pkcs7-certificate" },
    { ".p7c",   "application/x-pkcs7-mime" },
    { ".p7m",   "application/x-pkcs7-mime" },
    { ".p7r",   "application/x-pkcs7-certreqresp" },
    { ".p7s",   "application/x-pkcs7-signature" },
    { ".part",  "application/pro_eng" },
    { ".pbm",   "image/x-portable-bitmap" },
    { ".pcl",   "application/x-pcl" },
    { ".pct",   "image/x-pict" },
    { ".pcx",   "image/x-pcx" },
    { ".pdb",   "chemical/x-pdb" },
    { ".pdf",   "application/pdf" },
    { ".pfunk", "audio/make" },
    { ".pfx",   "application/x-pkcs12" },
    { ".pgm",   "image/x-portable-graymap" },
    { ".pic",   "image/pict" },
    { ".pict",  "image/pict" },
    { ".pkg",   "application/x-newton-compatible-pkg" },
    { ".pko",   "application/vnd.ms-pki.pko" },
    { ".pl",    "text/plain" },
    { ".plx",   "application/x-PiXCLscript" },
    { ".pm",    "image/x-xpixmap" },
    { ".pma",   "application/x-perfmon" },
    { ".pmc",   "application/x-perfmon" },
    { ".pml",   "application/x-perfmon" },
    { ".pmr",   "application/x-perfmon" },
    { ".pmw",   "application/x-perfmon" },
    { ".pm4",   "application/x-pagemaker" },
    { ".pm5",   "application/x-pagemaker" },
    { ".png",   "image/png" },
    { ".pnm",   "image/x-portable-anymap" },
    { ".pot",   "application/vnd.ms-powerpoint" },
    { ".pov",   "model/x-pov" },
    { ".ppa",   "application/vnd.ms-powerpoint" },
    { ".ppm",   "image/x-portable-pixmap" },
    { ".pps",   "application/vnd.ms-powerpoint" },
    { ".ppt",   "application/vnd.ms-powerpoint" },
    { ".ppz",   "application/vnd.ms-powerpoint" },
    { ".pre",   "application/x-freelance" },
    { ".prf",   "application/pics-rules" },
    { ".prt",   "application/pro_eng" },
    { ".ps",    "application/postscript" },
    { ".psd",   "application/octet-stream" },
    { ".pub",   "application/x-mspublisher" },
    { ".pvu",   "paleovu/x-pv" },
    { ".pwz",   "application/vnd.ms-powerpoint" },
    { ".py",    "text/x-script.python" },
    { ".pyc",   "application/x-bytecode.python" },
    { ".qcp",   "audio/vnd.qcelp" },
    { ".qd3",   "x-world/x-3dmf" },
    { ".qd3d",  "x-world/x-3dmf" },
    { ".qif",   "image/x-quicktime" },
    { ".qt",    "video/quicktime" },
    { ".qtc",   "video/x-qtc" },
    { ".qti",   "image/x-quicktime" },
    { ".qtif",  "image/quicktime" },
    { ".ra",    "audio/x-pn-realaudio" },
    { ".ram",   "audio/x-pn-realaudio" },
    { ".ras",   "image/x-cmu-raster" },
    { ".rast",  "image/x-cmu-raster" },
    { ".rdf",   "application/rdf+xml" },
    { ".rexx",  "text/x-script.rexx" },
    { ".rf",    "image/vnd.rn-realflash" },
    { ".rgb",   "image/x-rgb" },
    { ".rm",    "audio/x-pn-realaudio" },
    { ".rmi",   "audio/midi" },
    { ".rmm",   "audio/x-pn-realaudio" },
    { ".rmp",   "audio/x-pn-realaudio-plugin" },
    { ".rng",   "application/vnd.nokia.ringing-tone" },
    { ".rnx",   "application/vnd.rn-realplayer" },
    { ".roff",  "application/x-roff" },
    { ".rp",    "image/vnd.rn-realpix" },
    { ".rpm",   "application/octet-stream" },
    { ".rss",   "application/rss+xml" },
    { ".rt",    "text/richtext" },
    { ".rtf",   "text/rtf" },
    { ".rtx",   "text/richtext" },
    { ".rv",    "video/vnd.rn-realvideo" },
    { ".s",     "text/plain" },
    { ".s3m",   "audio/s3m" },
    { ".saveme","application/octet-stream" },
    { ".sbk",   "application/x-tbook" },
    { ".scd",   "application/x-msschedule" },
    { ".scm",   "video/x-scm" },
    { ".sct",   "text/scriptlet" },
    { ".sdml",  "text/plain" },
    { ".sdp",   "application/sdp" },
    { ".sdr",   "application/sounder" },
    { ".sea",   "application/sea" },
    { ".set",   "application/set" },
    { ".setpay","application/set-payment-initiation" },
    { ".setreg","application/set-registration-initiation" },
    { ".sgm",   "text/sgml" },
    { ".sgml",  "text/sgml" },
    { ".sh",    "application/x-sh" },
    { ".shar",  "application/x-shar" },
    { ".sid",   "audio/x-psid" },
    { ".sit",   "application/x-stuffit" },
    { ".skd",   "application/x-koan" },
    { ".skm",   "application/x-koan" },
    { ".skp",   "application/x-koan" },
    { ".skt",   "application/x-koan" },
    { ".sl",    "application/x-seelogo" },
    { ".smi",   "application/smil" },
    { ".smil",  "application/smil" },
    { ".snd",   "audio/basic" },
    { ".sol",   "application/solids" },
    { ".spc",   "application/x-pkcs7-certificate" },
    { ".spl",   "application/futuresplash" },
    { ".spr",   "application/x-sprite" },
    { ".sprite","application/x-sprite" },
    { ".spx",   "audio/ogg" },
    { ".sql",   "application/x-sql" },
    { ".src",   "application/x-wais-source" },
    { ".ssm",   "application/streamingmedia" },
    { ".sst",   "application/vnd.ms-pki.certstore" },
    { ".stc",   "application/vnd.sun.xml.calc.template" },
    { ".std",   "application/vnd.sun.xml.draw.template" },
    { ".step",  "application/step" },
    { ".sti",   "application/vnd.sun.xml.impress.template" },
    { ".stl",   "application/x-navistyle" },
    { ".stm",   "text/html" },
    { ".stp",   "application/step" },
    { ".stw",   "application/vnd.sun.xml.writer.template" },
    { ".sv4cpio", "application/x-sv4cpio" },
    { ".sv4crc","application/x-sv4crc" },
    { ".svf",   "image/vnd.dwg" },
    { ".svg",   "image/svg+xml" },
    { ".svgz",  "image/x-svgz" },
    { ".svr",   "application/x-world" },
    { ".swf",   "application/x-shockwave-flash" },
    { ".sxc",   "application/vnd.sun.xml.calc" },
    { ".sxd",   "application/vnd.sun.xml.draw" },
    { ".sxg",   "application/vnd.sun.xml.writer.global" },
    { ".sxi",   "application/vnd.sun.xml.impress" },
    { ".sxm",   "application/vnd.sun.xml.math" },
    { ".sxw",   "application/vnd.sun.xml.writer" },
    { ".t",     "application/x-troff" },
    { ".talk",  "text/x-speech" },
    { ".tar",   "application/x-tar" },
    { ".tbk",   "application/toolbook" },
    { ".tcl",   "text/plain" },
    { ".tcsh",  "text/x-script.tcsh" },
    { ".tex",   "application/x-tex" },
    { ".texi",  "application/x-texinfo" },
    { ".texinfo", "application/x-texinfo" },
    { ".text",  "text/plain" },
    { ".tgz",   "application/x-compressed" },
    { ".tif",   "image/tiff" },
    { ".tiff",  "image/tiff" },
    { ".torrent", "application/x-bittorrent" },
    { ".tr",    "application/x-troff" },
    { ".tsi",   "audio/tsp-audio" },
    { ".tsp",   "audio/tsplayer" },
    { ".tsv",   "text/tab-separated-values" },
    { ".turbot","image/florian" },
    { ".txt",   "text/plain" },
    { ".uil",   "text/x-uil" },
    { ".uni",   "text/uri-list" },
    { ".unis",  "text/uri-list" },
    { ".unv",   "application/i-deas" },
    { ".uri",   "text/uri-list" },
    { ".uris",  "text/uri-list" },
    { ".ustar", "application/x-ustar" },
    { ".uu",    "text/x-uuencode" },
    { ".uue",   "text/x-uuencode" },
    { ".uls",   "text/iuls" },
    { ".vcd",   "application/x-cdlink" },
    { ".vcf",   "text/x-vcard" },
    { ".vcs",   "text/x-vCalendar" },
    { ".vda",   "application/vda" },
    { ".vdo",   "video/vdo" },
    { ".vew",   "application/groupwise" },
    { ".viv",   "video/vnd.vivo" },
    { ".vivo",  "video/vnd.vivo" },
    { ".vmd",   "application/vocaltec-media-desc" },
    { ".vmf",   "application/vocaltect-media-file" },
    { ".voc",   "audio/voc" },
    { ".vos",   "video/vosaic" },
    { ".vox",   "audio/voxware" },
    { ".vqe",   "audio/x-twinvq-plugin" },
    { ".vqf",   "audio/x-twinvq" },
    { ".vql",   "audio/x-twinvq-plugin" },
    { ".vrml",  "x-world/x-vrml" },
    { ".vrt",   "x-world/x-vrt" },
    { ".vsd",   "application/vnd.visio" },
    { ".vst",   "application/x-visio" },
    { ".vsw",   "application/x-visio" },
    { ".wav",   "audio/x-wav" },
    { ".wcm",   "application/vnd.ms-works" },
    { ".wdb",   "application/vnd.ms-works" },
    { ".wks",   "application/vnd.ms-works" },
    { ".wmf",   "application/x-msmetafile" },
    { ".w60",   "application/wordperfect6.0" },
    { ".w61",   "application/wordperfect6.1" },
    { ".w6w",   "application/msword" },
    { ".wb1",   "application/x-qpro" },
    { ".wbmp",  "image/vnd.wap.wbmp" },
    { ".web",   "application/vnd.xara" },
    { ".wiz",   "application/msword" },
    { ".wk1",   "application/x-123" },
    { ".wma",   "application/x-ms-wma" },
    { ".wml",   "text/vnd.wap.wml" },
    { ".wmlc",  "application/vnd.wap.wmlc" },
    { ".wmls",  "text/vnd.wap.wmlscript" },
    { ".wmlsc", "application/vnd.wap.wmlscript" },
    { ".word",  "application/msword" },
    { ".wp",    "application/wordperfect" },
    { ".wp5",   "application/wordperfect" },
    { ".wp6",   "application/wordperfect" },
    { ".wpd",   "application/wordperfect" },
    { ".wq1",   "application/x-lotus" },
    { ".wps",   "application/vnd.ms-works" },
    { ".wri",   "application/x-mswrite" },
    { ".wrl",   "x-world/x-vrml" },
    { ".wrz",   "x-world/x-vrml" },
    { ".wsc",   "text/scriplet" },
    { ".wsrc",  "application/x-wais-source" },
    { ".wtk",   "application/x-wintalk" },
    { ".xaf",   "x-world/x-vrml" },
    { ".x-png", "image/png" },
    { ".xbm",   "image/x-xbitmap" },
    { ".xdr",   "video/x-amt-demorun" },
    { ".xgz",   "xgl/drawing" },
    { ".xif",   "image/vnd.xif" },
    { ".xht",   "application/xhtml+xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".xl",    "application/excel" },
    { ".xla",   "application/vnd.ms-excel" },
    { ".xlb",   "application/vnd.ms-excel" },
    { ".xlc",   "application/vnd.ms-excel" },
    { ".xld",   "application/vnd.ms-excel" },
    { ".xlk",   "application/vnd.ms-excel" },
    { ".xll",   "application/vnd.ms-excel" },
    { ".xlm",   "application/vnd.ms-excel" },
    { ".xls",   "application/vnd.ms-excel" },
    { ".xlt",   "application/vnd.ms-excel" },
    { ".xlv",   "application/vnd.ms-excel" },
    { ".xlw",   "application/vnd.ms-excel" },
    { ".xm",    "audio/xm" },
    { ".xml",   "application/xml" },
    { ".xmz",   "xgl/movie" },
    { ".xof",   "x-world/x-vrml" },
    { ".xpix",  "application/x-vnd.ls-xpix" },
    { ".xpm",   "image/x-xpixmap" },
    { ".xsl",   "application/xml" },
    { ".xslt",  "application/xslt+xml" },
    { ".xspf",  "application/xspf+xml" },
    { ".xsr",   "video/x-amt-showrun" },
    { ".xul",   "application/vnd.mozilla.xul+xml" },
    { ".xwd",   "image/x-windowdump" },
    { ".xyz",   "chemical/x-pdb" },
    { ".z",     "application/x-compressed" },
    { ".zip",   "application/x-zip-compressed" },
    { ".zoo",   "application/octet-stream" },
    { ".zsh",   "text/x-script.zsh" },

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
    int         i;
    static int  once = 0;

    if (!once) {
        once = 1;

        /*
         * Initialize hash table of file extensions.
         */

        Tcl_InitHashTable(&types, TCL_STRING_KEYS);

        /*
         * Add default system types first from above
         */

        for (i = 0; typetab[i].ext != NULL; ++i) {
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

    for (i=0; i < Ns_SetSize(set); i++) {
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
Ns_GetMimeType(CONST char *file)
{
    CONST char    *start, *ext;
    Ns_DString     ds;
    Tcl_HashEntry *hPtr;

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
NsTclGuessTypeObjCmd(ClientData dummy, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    CONST char *type;

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
NsGetMimeTypes(Ns_DString *dest)
{
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

    hPtr = Tcl_FirstHashEntry(&types, &search);
    while (hPtr != NULL) {
        Tcl_DStringAppendElement(dest, Tcl_GetHashKey(&types, hPtr));
        Tcl_DStringAppendElement(dest, Tcl_GetHashValue(hPtr));
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
    int             new;

    Ns_DStringInit(&ds);
    ext = LowerDString(&ds, ext);
    he = Tcl_CreateHashEntry(&types, ext, &new);
    if (new == 0) {
        ns_free(Tcl_GetHashValue(he));
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
        if (isupper(UCHAR(*p))) {
            *p = tolower(UCHAR(*p));
        }
        ++p;
    }

    return dsPtr->string;
}
