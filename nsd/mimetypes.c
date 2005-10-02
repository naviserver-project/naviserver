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
 * mimetypes.c --
 *
 *      Defines standard default mime types. 
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

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
    { ".ai",    "application/postscript" },
    { ".aif",   "audio/aiff" },
    { ".aifc",  "audio/aiff" },
    { ".aiff",  "audio/aiff" },
    { ".ani",   "application/x-navi-animation" },
    { ".art",   "image/x-art" },
    { ".asf",   "video/x-ms-asf" },
    { ".asr",   "video/x-ms-asf" },
    { ".asx",   "video/x-ms-asf" },
    { ".atom",  "application/atom+xml" },
    { ".au",    "audio/basic" },
    { ".avi",   "video/x-msvideo" },
    { ".bin",   "application/x-macbinary" },
    { ".bmp",   "image/bmp" },
    { ".cer",   "application/x-x509-ca-cert" },
    { ".class", "application/octet-stream" },
    { ".cpio",  "application/x-cpio" },
    { ".css",   "text/css" },
    { ".csv",   "text/csv" },
    { ".dcr",   "application/x-director" },
    { ".der",   "application/x-x509-ca-cert" },
    { ".dia",   "application/x-dia" },
    { ".dir",   "application/x-director" },
    { ".doc",   "application/msword" },
    { ".dot",   "application/msword" },
    { ".dp",    "application/commonground" },
    { ".dtd",   "application/xml-dtd" },
    { ".dxr",   "application/x-director" },
    { ".elm",   "text/plain" },
    { ".eml",   "text/plain" },
    { ".eps",   "application/postscript" },
    { ".exe",   "application/octet-stream" },
    { ".gbt",   "text/plain" },    
    { ".gif",   "image/gif" },
    { ".gz",    "application/x-compressed" },
    { ".h",     "text/plain" },
    { ".hqx",   "application/mac-binhex40" },
    { ".ico",   "image/x-icon" },
    { ".ica",   "application/x-ica" },
    { ".ics",   "text/calendar" },
    { ".ifb",   "text/calendar" },
    { ".jar",   "application/x-java-archive" },
    { ".jfif",  "image/jpeg" },
    { ".jng",   "image/x-jng" },
    { ".jpe",   "image/jpeg" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".js",    "application/x-javascript" },
    { ".ls",    "application/x-javascript" },
    { ".m3u",   "audio/x-mpegurl" },
    { ".m4a",   "audio/mp4" },
    { ".m4p",   "audio/mp4" },
    { ".man",   "application/x-troff-man" },
    { ".map",   "application/x-navimap" },
    { ".mdb",   "application/x-msaccess" },
    { ".mid",   "audio/x-midi" },
    { ".midi",  "audio/x-midi" },
    { ".mng",   "image/x-mng" },
    { ".mocha", "application/x-javascript" },
    { ".mov",   "video/quicktime" },
    { ".mp2",   "audio/mpeg" },
    { ".mp3",   "audio/mpeg" },
    { ".mp4",   "audio/mp4" },
    { ".mpe",   "video/mpeg" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".mpga",  "video/mpeg" },
    { ".mpv2",  "video/mpeg" },
    { ".mxu",   "video/vnd.mpegurl" },
    { ".nvd",   "application/x-navidoc" },
    { ".nvm",   "application/x-navimap" },
    { ".ogg",   "application/ogg" },
    { ".pbm",   "image/x-portable-bitmap" },
    { ".pdf",   "application/pdf" },
    { ".pgm",   "image/x-portable-graymap" },
    { ".pic",   "image/pict" },
    { ".pict",  "image/pict" },
    { ".pnm",   "image/x-portable-anymap" },
    { ".png",   "image/png" },
    { ".pot",   "application/vnd.ms-powerpoint"},
    { ".pps",   "application/vnd.ms-powerpoint"},
    { ".ppt",   "application/vnd.ms-powerpoint"},
    { ".ps",    "application/postscript" },
    { ".pub",   "application/x-mspubllisher" },
    { ".qt",    "video/quicktime" },
    { ".ra",    "audio/x-pn-realaudio" },
    { ".ram",   "audio/x-pn-realaudio" },
    { ".ras",   "image/x-cmu-raster" },
    { ".rdf",   "application/rdf+xml" },
    { ".rgb",   "image/x-rgb" },
    { ".rtf",   "text/rtf" },
    { ".rtx",   "text/richtext" },
    { ".rss",   "application/rss+xml" },
    { ".sit",   "application/x-stuffit" },
    { ".smi",   "application/smil" },
    { ".smil",  "application/smil" },
    { ".snd",   "audio/basic" },
    { ".spx",   "application/ogg" },
    { ".sql",   "application/x-sql" },
    { ".stc",   "application/vnd.sun.xml.calc.template" },
    { ".std",   "application/vnd.sun.xml.draw.template" },
    { ".sti",   "application/vnd.sun.xml.impress.template" },
    { ".stl",   "application/x-navistyle" },
    { ".stm",   "text/html" },
    { ".stw",   "application/vnd.sun.xml.writer.template" },
    { ".svg",   "image/svg+xml" },
    { ".svgz",  "image/x-svgz" },
    { ".swf",   "application/x-shockwave-flash" },
    { ".sxc",   "application/vnd.sun.xml.calc" },
    { ".sxd",   "application/vnd.sun.xml.draw" },
    { ".sxg",   "application/vnd.sun.xml.writer.global" },
    { ".sxi",   "application/vnd.sun.xml.impress" },
    { ".sxm",   "application/vnd.sun.xml.math" },
    { ".sxw",   "application/vnd.sun.xml.writer" },
    { ".tar",   "application/x-tar" },
    { ".tcl",   "text/plain" },
    { ".text",  "text/plain" },
    { ".tgz",   "application/x-compressed" },
    { ".tif",   "image/tiff" },
    { ".tiff",  "image/tiff" },
    { ".torrent", "application/x-bittorrent" },
    { ".tsv",   "text/tab-separated-values" },
    { ".txt",   "text/plain" },
    { ".vcf",   "text/x-vcard" },
    { ".xbm",   "image/x-xbitmap" },
    { ".xpm",   "image/x-xpixmap" },
    { ".xht",   "application/xhtml+xml"},
    { ".xhtml", "application/xhtml+xml"},
    { ".xla",   "application/vnd.ms-excel"},
    { ".xlc",   "application/vnd.ms-excel"},
    { ".xlm",   "application/vnd.ms-excel"},
    { ".xls",   "application/vnd.ms-excel"},
    { ".xlt",   "application/vnd.ms-excel"},
    { ".xlw",   "application/vnd.ms-excel"},
    { ".xml",   "application/xml"},
    { ".xsl",   "application/xml"},
    { ".xslt",  "application/xslt+xml"},
    { ".xul",   "application/vnd.mozilla.xul+xml"},
    { ".vrml",  "x-world/x-vrml" },
    { ".wav",   "audio/x-wav" },
    { ".wrl",   "x-world/x-vrml" },
    { ".z",     "application/x-compressed" },
    { ".zip",   "application/x-zip-compressed" },
    { NULL,     NULL }
};


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
 * NsInitMimeTypes --
 *
 *      Add compiled-in default mime types. 
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
NsInitMimeTypes(void)
{
    int i;

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


/*
 *----------------------------------------------------------------------
 *
 * NsUpdateMimeTypes --
 *
 *      Add configured mime types. 
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
NsUpdateMimeTypes(void)
{
    Ns_Set *set;
    int     i;

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
    Tcl_SetStringObj(Tcl_GetObjResult(interp), type, -1);

    return TCL_OK;
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
