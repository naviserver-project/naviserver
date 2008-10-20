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
 * tclmisc.c --
 *
 *	Implements a lot of Tcl API commands.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

/*
 * Local functions defined in this file
 */

static int WordEndsInSemi(char *ip);


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclPrintfResult --
 *
 *      Leave a formatted message in the given Tcl interps result.
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
Ns_TclPrintfResult(Tcl_Interp *interp, char *fmt, ...)
{
    va_list     ap;
    Tcl_DString ds;

    Tcl_DStringInit(&ds);
    va_start(ap, fmt);
    Ns_DStringVPrintf(&ds, fmt, ap);
    va_end(ap);
    Tcl_DStringResult(interp, &ds);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRunOnceObjCmd --
 *
 *      Implements ns_runonce.  Run the given script only once.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on script.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRunOnceObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                   Tcl_Obj *CONST objv[])
{
    NsInterp             *itPtr = arg;
    CONST char           *script;
    int                   new, global = NS_FALSE;
    static Tcl_HashTable  runTable;
    static int            initialized;

    Ns_ObjvSpec opts[] = {
        {"-global", Ns_ObjvBool,  &global, (void *) NS_TRUE},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"script", Ns_ObjvString, &script, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_MasterLock();
    if (!initialized) {
        Tcl_InitHashTable(&runTable, TCL_STRING_KEYS);
        initialized = NS_TRUE;
    }
    (void) Tcl_CreateHashEntry(global ? &runTable :
                               &itPtr->servPtr->tcl.runTable, script, &new);
    Ns_MasterUnlock();

    if (new) {
        return Tcl_Eval(interp, script);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogErrorInfo --
 *
 *      Log the global errorInfo variable to the server log along with
 *      some connection info, if available.
 *
 * Results:
 *      Returns a read-only pointer to the complete errorInfo.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Ns_TclLogErrorInfo(Tcl_Interp *interp, CONST char *extraInfo)
{
    NsInterp    *itPtr = NsGetInterpData(interp);
    Ns_Conn     *conn;
    CONST char  *errorInfo, **logHeaders, **hdr;
    char        *value;
    Ns_DString   ds;

    if (extraInfo != NULL) {
        Tcl_AddObjErrorInfo(interp, extraInfo, -1);
    }
    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if (errorInfo == NULL) {
        errorInfo = "";
    }
    if (itPtr != NULL && itPtr->conn != NULL) {
        conn = itPtr->conn;
        Ns_DStringInit(&ds);
        if (conn->request->method != NULL) {
            Ns_DStringVarAppend(&ds, conn->request->method, " ", NULL);
        }
        if (conn->request->url != NULL) {
            Ns_DStringVarAppend(&ds, conn->request->url, ", ", NULL);
        }
        Ns_DStringVarAppend(&ds, "PeerAddress: ", Ns_ConnPeer(conn), NULL);

        logHeaders = itPtr->servPtr->tcl.errorLogHeaders;
        if (logHeaders != NULL) {
            for (hdr = logHeaders; *hdr != NULL; hdr++) {
                if ((value = Ns_SetIGet(conn->headers, *hdr)) != NULL) {
                    Ns_DStringVarAppend(&ds, ", ", *hdr, ": ", value, NULL);
                }
            }
        }
        Ns_Log(Error, "%s\n%s", Ns_DStringValue(&ds), errorInfo);
        Ns_DStringFree(&ds);
    } else {
        Ns_Log(Error, "%s\n%s", Tcl_GetStringResult(interp), errorInfo);
    }

   return errorInfo;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogError --
 *
 *      Log the global errorInfo variable to the server log.
 *
 * Results:
 *      Returns a read-only pointer to the errorInfo.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Ns_TclLogError(Tcl_Interp *interp)
{
    return Ns_TclLogErrorInfo(interp, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogErrorRequest --
 *
 *      Deprecated.  See: Ns_TclLoggErrorInfo.
 *
 * Results:
 *      Returns a pointer to the read-only errorInfo.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *conn)
{
    return Ns_TclLogErrorInfo(interp, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclStripHtmlCmd --
 *
 *      Implements ns_striphtml.
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
NsTclStripHtmlCmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    int   intag;     /* flag to see if are we inside a tag */
    int   intspec;   /* flag to see if we are inside a special char */
    char *inString;  /* copy of input string */
    char *inPtr;     /* moving pointer to input string */
    char *outPtr;    /* moving pointer to output string */

    if (argc != 2) {
        Tcl_AppendResult(interp, "wrong # of args:  should be \"",
                         argv[0], " page\"", NULL);
        return TCL_ERROR;
    }

    /*
     * Make a copy of the input and point the moving and output ptrs to it.
     */
    inString = ns_strdup(argv[1]);
    inPtr    = inString;
    outPtr   = inString;
    intag    = 0;
    intspec  = 0;

    while (*inPtr != '\0') {

        if (*inPtr == '<') {
            intag = 1;

        } else if (intag && (*inPtr == '>')) {
        /* inside a tag that closes */
            intag = 0;

        } else if (intspec && (*inPtr == ';')) {
        /* inside a special character that closes */
            intspec = 0;

        } else if (!intag && !intspec) {
        /* regular text */

            if (*inPtr == '&') {
        /* starting a new special character */
                intspec=WordEndsInSemi(inPtr);
        }

            if (!intspec) {
        /* incr pointer only if we're not in something htmlish */
                *outPtr++ = *inPtr;
        }
        }
        ++inPtr;
    }

    /* null-terminator */
    *outPtr = '\0';

    Tcl_SetResult(interp, inString, TCL_VOLATILE);

    ns_free(inString);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCryptObjCmd --
 *
 *      Implements ns_crypt as ObjCommand.
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
NsTclCryptObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                 Tcl_Obj *CONST objv[])
{
    char buf[NS_ENCRYPT_BUFSIZE];

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "key salt");
        return TCL_ERROR;
    }
    Tcl_SetResult(interp,
                  Ns_Encrypt(Tcl_GetString(objv[1]),
                             Tcl_GetString(objv[2]), buf), TCL_VOLATILE);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHrefsCmd --
 *
 *      Implements ns_hrefs.
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
NsTclHrefsCmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    char *p, *s, *e, *he, save;

    if (argc != 2) {
        Tcl_AppendResult(interp, "wrong # args: should be \"",
                         argv[0], " html\"", (char *) NULL);
        return TCL_ERROR;
    }

    p = argv[1];
    while ((s = strchr(p, '<')) && (e = strchr(s, '>'))) {
        ++s;
        *e = '\0';
        while (*s && isspace(UCHAR(*s))) {
            ++s;
        }
        if ((*s == 'a' || *s == 'A') && isspace(UCHAR(s[1]))) {
            ++s;
            while (*s) {
                if (!strncasecmp(s, "href", 4)) {
                    s += 4;
                    while (*s && isspace(UCHAR(*s))) {
                        ++s;
                    }
                    if (*s == '=') {
                        ++s;
                        while (*s && isspace(UCHAR(*s))) {
                            ++s;
                        }
                        he = NULL;
                        if (*s == '\'' || *s == '"') {
                            he = strchr(s+1, *s);
                            ++s;
                        }
                        if (he == NULL) {
                            he = s;
                            while (!isspace(UCHAR(*he))) {
                                ++he;
                            }
                        }
                        save = *he;
                        *he = '\0';
                        Tcl_AppendElement(interp, s);
                        *he = save;
                        break;
                    }
                }
                if (*s == '\'' || *s == '\"') {
                    while (*s && (*s != '\'' || *s != '\"')) {
                        ++s;
                    }
                    continue;
                }
                ++s;
            }
        }
        *e++ = '>';
        p = e;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHTUUEncodeObjCmd --
 *
 *      Implements ns_uuencode as obj command.
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
NsTclHTUUEncodeObjCmd(ClientData dummy, Tcl_Interp *interp, int objc,
                      Tcl_Obj **objv)
{
    unsigned char *string;
    char          *result;
    int            nbytes;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    string = Tcl_GetByteArrayFromObj(objv[1], &nbytes);
    result = ns_malloc((size_t) 1 + (4 * MAX(nbytes,2)) / 2);
    Ns_HtuuEncode(string, (size_t)nbytes, result);
    Tcl_SetResult(interp, result, (Tcl_FreeProc *) ns_free);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HTUUDecodeObjcmd --
 *
 *      Implements ns_uudecode as obj command.
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
NsTclHTUUDecodeObjCmd(ClientData dummy, Tcl_Interp *interp, int objc,
                      Tcl_Obj **objv)
{
    int            size;
    char          *string;
    unsigned char *decoded;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    string = Tcl_GetStringFromObj(objv[1], &size);
    size += 3;
    decoded = ns_malloc(size);
    size = Ns_HtuuDecode(string, decoded, size);
    decoded[size] = '\0';
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(decoded, size));
    ns_free(decoded);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCrashCmd --
 *
 *      Crash the server to test exception handling.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server will segfault.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCrashCmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    char *death;

    death = NULL;
    *death = 1;

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * WordEndsInSemi --
 *
 *      Does this word end in a semicolon or a space?
 *
 * Results:
 *      1 if semi, 0 if space.
 *
 * Side effects:
 *      Undefined behavior if string does not end in null
 *
 *----------------------------------------------------------------------
 */

static int
WordEndsInSemi(char *ip)
{
    if (ip == NULL) {
        return 0;
    }
    /* advance past the first '&' so we can check for a second
       (i.e. to handle "ben&jerry&nbsp;")
    */
    if (*ip == '&') {
        ip++;
    }
    while((*ip != '\0') && (*ip != ' ') && (*ip != ';') && (*ip != '&')) {
        ip++;
    }
    if (*ip == ';') {
        return 1;
    } else {
        return 0;
    }
}

/*
 *  The SHA1 routines are borrowed from libmd:
 *
 *  * sha.c - NIST Secure Hash Algorithm, FIPS PUB 180 and 180.1.
 *  * The algorithm is by spook(s) unknown at the U.S. National Security Agency.
 *  *
 *  * Written 2 September 1992, Peter C. Gutmann.
 *  * This implementation placed in the public domain.
 *  *
 *  * Modified 1 June 1993, Colin Plumb.
 *  * Modified for the new SHS based on Peter Gutmann's work,
 *  * 18 July 1994, Colin Plumb.
 *  *
 *  * Renamed to SHA and comments updated a bit 1 November 1995, Colin Plumb.
 *  * These modifications placed in the public domain.
 *  *
 *  * Comments to pgut1@cs.aukuni.ac.nz
 *  *
 *  * Hacked for use in libmd by Martin Hinner <mhi@penguin.cz>
 *
 *  This Tcl library was hacked by Jon Salz <jsalz@mit.edu>.
 *
 */

static char hexChars[] = "0123456789ABCDEF";

/*
 * Define to 1 for FIPS 180.1 version (with extra rotate in prescheduling),
 * 0 for FIPS 180 version (with the mysterious "weakness" that the NSA
 * isn't talking about).
 */

#define SHA_VERSION 1

#define SHA_BLOCKBYTES 64
#define SHA_HASHBYTES 20

/*
   Shuffle the bytes into big-endian order within words, as per the
   SHA spec.
 */
static void
shaByteSwap (uint32_t * dest, uint8_t const *src, unsigned int words)
{
    do {
       *dest++ = (uint32_t) ((unsigned) src[0] << 8 | src[1]) << 16 |
	         ((unsigned) src[2] << 8 | src[3]);
       src += 4;
    } while (--words);
}

/* Initialize the SHA values */
void Ns_CtxSHAInit (Ns_CtxSHA1 * ctx)
{

    /* Set the h-vars to their initial values */
    ctx->iv[0] = 0x67452301;
    ctx->iv[1] = 0xEFCDAB89;
    ctx->iv[2] = 0x98BADCFE;
    ctx->iv[3] = 0x10325476;
    ctx->iv[4] = 0xC3D2E1F0;

    /* Initialise bit count */
#ifdef HAVE64
    ctx->bytes = 0;
#else
    ctx->bytesHi = 0;
    ctx->bytesLo = 0;
#endif
}

/*
   The SHA f()-functions. The f1 and f3 functions can be optimized to
   save one boolean operation each - thanks to Rich Schroeppel,
   rcs@cs.arizona.edu for discovering this.
   The f3 function can be modified to use an addition to combine the
   two halves rather than OR, allowing more opportunity for using
   associativity in optimization. (Colin Plumb)

   Note that it may be necessary to add parentheses to these macros
   if they are to be called with expressions as arguments.
 */
#define f1(x,y,z) ( z ^ (x & (y ^ z) ) )	/* Rounds 0-19 */
#define f2(x,y,z) ( x ^ y ^ z )			/* Rounds 20-39 */
#define f3(x,y,z) ( (x & y) + (z & (x ^ y) ) )	/* Rounds 40-59 */
#define f4(x,y,z) ( x ^ y ^ z )			/* Rounds 60-79 */

/* The SHA Mysterious Constants. */
#define K2 0x5A827999L		/* Rounds 0 -19 - floor(sqrt(2)  * 2^30) */
#define K3 0x6ED9EBA1L		/* Rounds 20-39 - floor(sqrt(3)  * 2^30) */
#define K5 0x8F1BBCDCL		/* Rounds 40-59 - floor(sqrt(5)  * 2^30) */
#define K10 0xCA62C1D6L		/* Rounds 60-79 - floor(sqrt(10) * 2^30) */

/* 32-bit rotate left - kludged with shifts */
#define ROTL(n,X) ( (X << n) | (X >> (32-n)) )

/*
   The initial expanding function

   The hash function is defined over an 80-word expanded input array W,
   where the first 16 are copies of the input data, and the remaining 64
   are defined by W[i] = W[i-16] ^ W[i-14] ^ W[i-8] ^ W[i-3]. This
   implementation generates these values on the fly in a circular buffer.

   The new "corrected" FIPS 180.1 added a 1-bit left rotate to this
   computation of W[i].

   The expandx() version doesn't write the result back, which can be
   used for the last three rounds since those outputs are never used.
 */
#if SHA_VERSION			/* FIPS 180.1 */

#define expandx(W,i) (t = W[i&15] ^ W[(i-14)&15] ^ W[(i-8)&15] ^ W[(i-3)&15],\
			ROTL(1, t))
#define expand(W,i) (W[i&15] = expandx(W,i))

#else /* Old FIPS 180 */

#define expandx(W,i) (W[i&15] ^ W[(i-14)&15] ^ W[(i-8)&15] ^ W[(i-3)&15])
#define expand(W,i) (W[i&15] ^= W[(i-14)&15] ^ W[(i-8)&15] ^ W[(i-3)&15])a

#endif /* SHA_VERSION */

/*
   The prototype SHA sub-round

   The fundamental sub-round is
   a' = e + ROTL(5,a) + f(b, c, d) + k + data;
   b' = a;
   c' = ROTL(30,b);
   d' = c;
   e' = d;
   ... but this is implemented by unrolling the loop 5 times and renaming
   the variables (e,a,b,c,d) = (a',b',c',d',e') each iteration.
 */
#define subRound(a, b, c, d, e, f, k, data) \
	 ( e += ROTL(5,a) + f(b, c, d) + k + data, b = ROTL(30, b) )
/*
   The above code is replicated 20 times for each of the 4 functions,
   using the next 20 values from the W[] array for "data" each time.
 */

/*
   Perform the SHA transformation. Note that this code, like MD5, seems to
   break some optimizing compilers due to the complexity of the expressions
   and the size of the basic block. It may be necessary to split it into
   sections, e.g. based on the four subrounds

   Note that this corrupts the sha->key area.
 */
static void
SHATransform(Ns_CtxSHA1 *sha)
{
    register uint32_t A, B, C, D, E;
#if SHA_VERSION
    register uint32_t t;
#endif

    /* Set up first buffer */
    A = sha->iv[0];
    B = sha->iv[1];
    C = sha->iv[2];
    D = sha->iv[3];
    E = sha->iv[4];

    /* Heavy mangling, in 4 sub-rounds of 20 interations each. */
    subRound (A, B, C, D, E, f1, K2, sha->key[0]);
    subRound (E, A, B, C, D, f1, K2, sha->key[1]);
    subRound (D, E, A, B, C, f1, K2, sha->key[2]);
    subRound (C, D, E, A, B, f1, K2, sha->key[3]);
    subRound (B, C, D, E, A, f1, K2, sha->key[4]);
    subRound (A, B, C, D, E, f1, K2, sha->key[5]);
    subRound (E, A, B, C, D, f1, K2, sha->key[6]);
    subRound (D, E, A, B, C, f1, K2, sha->key[7]);
    subRound (C, D, E, A, B, f1, K2, sha->key[8]);
    subRound (B, C, D, E, A, f1, K2, sha->key[9]);
    subRound (A, B, C, D, E, f1, K2, sha->key[10]);
    subRound (E, A, B, C, D, f1, K2, sha->key[11]);
    subRound (D, E, A, B, C, f1, K2, sha->key[12]);
    subRound (C, D, E, A, B, f1, K2, sha->key[13]);
    subRound (B, C, D, E, A, f1, K2, sha->key[14]);
    subRound (A, B, C, D, E, f1, K2, sha->key[15]);
    subRound (E, A, B, C, D, f1, K2, expand (sha->key, 16));
    subRound (D, E, A, B, C, f1, K2, expand (sha->key, 17));
    subRound (C, D, E, A, B, f1, K2, expand (sha->key, 18));
    subRound (B, C, D, E, A, f1, K2, expand (sha->key, 19));

    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 20));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 21));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 22));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 23));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 24));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 25));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 26));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 27));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 28));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 29));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 30));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 31));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 32));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 33));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 34));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 35));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 36));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 37));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 38));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 39));

    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 40));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 41));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 42));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 43));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 44));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 45));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 46));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 47));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 48));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 49));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 50));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 51));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 52));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 53));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 54));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 55));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 56));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 57));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 58));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 59));

    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 60));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 61));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 62));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 63));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 64));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 65));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 66));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 67));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 68));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 69));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 70));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 71));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 72));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 73));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 74));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 75));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 76));
    subRound (D, E, A, B, C, f4, K10, expandx (sha->key, 77));
    subRound (C, D, E, A, B, f4, K10, expandx (sha->key, 78));
    subRound (B, C, D, E, A, f4, K10, expandx (sha->key, 79));

    /* Build message digest */
    sha->iv[0] += A;
    sha->iv[1] += B;
    sha->iv[2] += C;
    sha->iv[3] += D;
    sha->iv[4] += E;
}

/* Update SHA for a block of data. */
void Ns_CtxSHAUpdate(Ns_CtxSHA1 *ctx, const unsigned char *buf, unsigned len)
{
    unsigned i;

    /* Update bitcount */

#ifdef HAVE64
    i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
    ctx->bytes += len;
#else
    uint32_t t = ctx->bytesLo;
    if ((ctx->bytesLo = t + len) < t) {
       ctx->bytesHi++;		/* Carry from low to high */
    }

    i = (unsigned) t % SHA_BLOCKBYTES;	/* Bytes already in ctx->key */
#endif

    /* i is always less than SHA_BLOCKBYTES. */
    if (SHA_BLOCKBYTES - i > len) {
        memcpy ((uint8_t *) ctx->key + i, buf, len);
        return;
    }

    if (i) {				/* First chunk is an odd size */
        memcpy ((uint8_t *) ctx->key + i, buf, SHA_BLOCKBYTES - i);
        shaByteSwap (ctx->key, (uint8_t *) ctx->key, SHA_BLOCKWORDS);
        SHATransform (ctx);
        buf += SHA_BLOCKBYTES - i;
        len -= SHA_BLOCKBYTES - i;
    }

    /* Process data in 64-byte chunks */
    while (len >= SHA_BLOCKBYTES) {
        shaByteSwap (ctx->key, buf, SHA_BLOCKWORDS);
        SHATransform (ctx);
        buf += SHA_BLOCKBYTES;
        len -= SHA_BLOCKBYTES;
    }

    /* Handle any remaining bytes of data. */
    if (len) {
       memcpy (ctx->key, buf, len);
    }
}

/*
   * Final wrapup - pad to 64-byte boundary with the bit pattern
   * 1 0* (64-bit count of bits processed, MSB-first)
 */

void Ns_CtxSHAFinal(Ns_CtxSHA1 *ctx, unsigned char digest[20])
{
#if HAVE64
    unsigned i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
#else
    unsigned i = (unsigned) ctx->bytesLo % SHA_BLOCKBYTES;
#endif
    uint8_t *p = (uint8_t *) ctx->key + i;	/* First unused byte */
    uint32_t t;

    /* Set the first char of padding to 0x80. There is always room. */
    *p++ = 0x80;

    /* Bytes of padding needed to make 64 bytes (0..63) */
    i = SHA_BLOCKBYTES - 1 - i;

    if (i < 8) {				/* Padding forces an extra block */
        memset (p, 0, i);
        shaByteSwap (ctx->key, (uint8_t *) ctx->key, 16);
        SHATransform (ctx);
        p = (uint8_t *) ctx->key;
        i = 64;
    }
    memset (p, 0, i - 8);
    shaByteSwap (ctx->key, (uint8_t *) ctx->key, 14);

    /* Append length in bits and transform */
#if HAVE64
    ctx->key[14] = (uint32_t) (ctx->bytes >> 29);
    ctx->key[15] = (uint32_t) ctx->bytes << 3;
#else
    ctx->key[14] = ctx->bytesHi << 3 | ctx->bytesLo >> 29;
    ctx->key[15] = ctx->bytesLo << 3;
#endif
    SHATransform (ctx);

    memcpy (digest, ctx->iv, sizeof (digest));
    for (i = 0; i < SHA_HASHWORDS; i++) {
        t = ctx->iv[i];
        digest[i * 4 + 0] = (uint8_t) (t >> 24);
        digest[i * 4 + 1] = (uint8_t) (t >> 16);
        digest[i * 4 + 2] = (uint8_t) (t >> 8);
        digest[i * 4 + 3] = (uint8_t) t;
    }

    memset(ctx, 0, sizeof(ctx)); 			/* In case it's sensitive */
}

void Ns_CtxString(unsigned char *digest, char *buf, int size)
{
    int i;

    for (i = 0; i < size; ++i) {
        buf[i * 2] = hexChars[digest[i] >> 4];
        buf[i * 2 + 1] = hexChars[digest[i] & 0xF];
    }
    buf[size * 2] = '\0';
}


/*
 *----------------------------------------------------------------------
 *
 * SHA1Cmd --
 *
 *      Returns a 40-character, hex-encoded string containing the SHA1
 *      hash of the first argument.
 *
 * Results:
 *	NS_OK
 *
 * Side effects:
 *	Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSHA1ObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                Tcl_Obj *CONST objv[])
{
    Ns_CtxSHA1     ctx;
    unsigned char  digest[20];
    char           digestChars[41];
    char          *str;
    int            strLen;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    str = Tcl_GetStringFromObj(objv[1],&strLen);
    Ns_CtxSHAInit(&ctx);
    Ns_CtxSHAUpdate(&ctx, (unsigned char *) str, (unsigned int) strLen);
    Ns_CtxSHAFinal(&ctx, digest);

    Ns_CtxString(digest, digestChars, 20);
    Tcl_AppendResult(interp, digestChars, NULL);

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileStatCmd --
 *
 *      Works as file stat command but uses native call when Tcl VFS is
 *      not compiled. The reason for this when native calls are used for speed,
 *      having still slow file stat does not help, need to use native call
 *      and file stat is the most used command
 *
 * Results:
 *	NS_OK
 *
 * Side effects:
 *	Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclFileStatObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                Tcl_Obj *CONST objv[])
{
    struct stat st;
    char *name;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "file ?varname?");
        return TCL_ERROR;
    }
    if (stat(Tcl_GetString(objv[1]), &st) != 0) {
        Tcl_SetResult(interp, "0", TCL_STATIC);
        return NS_OK;
    }
    if (objc > 2) {
        name = Tcl_GetString(objv[2]);
        Tcl_SetVar2Ex(interp, name, "dev", Tcl_NewIntObj(st.st_ino), 0);
        Tcl_SetVar2Ex(interp, name, "ino", Tcl_NewWideIntObj(st.st_ino), 0);
        Tcl_SetVar2Ex(interp, name, "nlink", Tcl_NewLongObj(st.st_nlink), 0);
        Tcl_SetVar2Ex(interp, name, "uid", Tcl_NewIntObj(st.st_uid), 0);
        Tcl_SetVar2Ex(interp, name, "gid", Tcl_NewIntObj(st.st_gid), 0);
        Tcl_SetVar2Ex(interp, name, "size", Tcl_NewWideIntObj(st.st_size), 0);
        Tcl_SetVar2Ex(interp, name, "atime", Tcl_NewLongObj(st.st_atime), 0);
        Tcl_SetVar2Ex(interp, name, "ctime", Tcl_NewLongObj(st.st_ctime), 0);
        Tcl_SetVar2Ex(interp, name, "mtime", Tcl_NewLongObj(st.st_mtime), 0);
        Tcl_SetVar2Ex(interp, name, "mode", Tcl_NewIntObj(st.st_mode), 0);
        Tcl_SetVar2Ex(interp, name, "type", Tcl_NewStringObj(
                  (S_ISREG(st.st_mode) ? "file" :
                        S_ISDIR(st.st_mode) ? "directory" :
#ifdef S_ISCHR
                          S_ISCHR(st.st_mode) ? "characterSpecial" :
#endif
#ifdef S_ISBLK
                            S_ISBLK(st.st_mode) ? "blockSpecial" :
#endif
#ifdef S_ISFIFO
                              S_ISFIFO(st.st_mode) ? "fifo" :
#endif
#ifdef S_ISLNK
                                S_ISLNK(st.st_mode) ? "link" :
#endif
#ifdef S_ISSOCK
                                  S_ISSOCK(st.st_mode) ? "socket" :
#endif
                   ""), -1), 0);
    }
    Tcl_SetResult(interp, "1", TCL_STATIC);
    return NS_OK;
}

/* $Id$
 *
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 *
 * $Log$
 * Revision 1.33  2008/10/20 00:06:54  seryakov
 * Driver changed, no request hacks, request can be empty, ns_conn content returns binary now
 *
 * Revision 1.32  2008/08/29 20:43:07  seryakov
 * Revert ns_filestat to not raise exception and use native system call
 *
 * Revision 1.31  2008/08/28 22:28:51  sdeasey
 * 	* configure.in:
 * 	* nsd/nsd.h:
 * 	* nsd/fastpath.c: Remove calls to Tcl's VFS routines within the
 * 	fastpath, as it adds a lot of noise. For starkit users there is
 * 	a new nsvfs module which provides pure Tcl VFS access to the file
 * 	system for the fastpath, and wrappers for ns_return and
 * 	ns_respond. This is actually more flexible: you can wrapped access
 * 	to /admin*, for bundled starkit files, and native access to
 * 	/fast*, for user added content.
 * 	* nsd/rollfile.c:
 * 	* nsd/tclmisc.c: Replace calls to the NsFast* wrappers with
 * 	straight Tcl_* calls.
 *
 * Revision 1.30  2008/03/09 07:34:48  seryakov
 *         Port to Win32 platform using Mingw32/Msys environment. Msys is what Tcl project using, same
 *         basic archive is what needed to conpile navoiserber for win32. Works stable as oppose to
 *         MSVC compiled code which used to crash for unknown reasons. Same configure script is used
 *         and config.h.
 *
 * Revision 1.29  2007/11/18 04:58:20  seryakov
 * Bad move, revert NsFastStat changes back, windwos port just needs proper
 * flags setup to work with the same stat or stat64 structures
 *
 * Revision 1.28  2007/11/18 04:23:36  seryakov
 *         * nsd/nsd.h:
 *         * nsd/fastpath.c: Added private function NsFastAllocStatBuf
 *         to follow Tcl VFS API
 *
 * Revision 1.27  2007/11/18 00:24:13  seryakov
 *         * include/Makefile.win32:
 *         Make sure src incude goes first
 *
 *         * nsd/tclmisc.c:
 *         * nsd/rollfile.c:
 *         * nsd/fastpath.c: Use Tcl_allocStatBuf for Tcl_FSStat to be
 *         portable. On Windows using static struct corrupts stack.
 *
 * Revision 1.26  2007/11/14 00:50:33  seryakov
 *         * include/nsthread.h:
 *         * include/Makefile.module
 *         * include/Makefile.win32
 *         * nsthread/winthread.c:
 *         * nsthread/Makefile:
 *         * nsd/Makefile:
 *         * nscp/Makefile:
 *         * nssock/Makefile:
 *         * nsdb/Makefile:
 *         * nscgi/Makefile:
 *         * nslog/Makefile:
 *         * nsd/nsmain.c:
 *         * nsd/tcltime.c:
 *         * nsd/tclmisc.c: Initial Windows port, still work in progress.
 *         New makefile added to perform compiling fromcommand line using the same top
 *         level makefile. To compile for windows, copy include/Makefile.win32
 *         into include/Makefile.module, the rest stays the same, only every module
 *         needs to be compiled separately, root Makefile is still unix specific.
 *
 * Revision 1.25  2007/10/21 15:44:39  seryakov
 *         * nsd/nswin32.c: No need for extra stubs
 *         * include/ns.h:
 *         * nsproxy/nsproxylib.c:
 *         * nsd/nsmain.c:
 *         * nsd/unix.c: New public API functions Ns_SetUser/Ns_SetGroup instead of
 *         deleted Ns_SetPriveleges. New Tcl commands ns_setuser/ns_setgroup instead of
 *         ns_setpriveleges
 *
 * Revision 1.24  2007/10/20 17:20:32  seryakov
 *     * include/ns.h:
 *     * nsd/unix.c: Added 2 new public API functions Ns_SetPriveleges and Ns_GetPriveleges
 *       which are resolve and assign user uid/gid
 *
 *     * nsproxy/nsproxylib.c:
 *     * nsd/nsmain.c: Switched to use new function that assign user real uid/gid
 *
 *     * nsd/tclmisc.c: New Tcl command ns_setpriveleges that sets real uid and gid
 *
 * Revision 1.23  2007/08/14 08:49:10  vasiljevic
 * Replaced uint32 with uint32_t.
 *
 * Revision 1.22  2007/05/21 05:28:39  seryakov
 *         * include/ns.h:
 *         * nsd/tclmisc.c: Added public domain MD5 implementation, new command ns_md5 created,
 *         new C API public functions added:
 *                Ns_CtxMD5Init(Ns_CtxMD5 *ctx)
 *                Ns_CtxMD5Update(Ns_CtxMD5 *ctx, unsigned const char *buf, unsigned len)
 *                Ns_CtxMD5Final(unsigned char digest[16], Ns_CtxMD5 *ctx)
 *                Ns_CtxSHAInit(Ns_CtxSHA1 *ctx)
 *                Ns_CtxSHAUpdate(Ns_CtxSHA1 *ctx, const unsigned char *buf, unsigned len)
 *                Ns_CtxSHAFinal(unsigned char digest[20], Ns_CtxSHA1 *ctx)
 *
 *         * nsperm/nsperm.c: Added first pieces to support both Basic and Digest
 *         authentications
 *
 * Revision 1.2  2000/09/11 05:13:24  vlad
 * *** empty log message ***
 *
 * Revision 1.1.1.1  1999/08/19 13:13:26  aland
 * 	Start of the pam_radius module
 *
 * Revision 1.2  1998/04/03 20:19:21  aland
 * now builds cleanly on Solaris 2.6
 *
 * Revision 1.1  1998/04/03 19:36:59  aland
 * oh yeah, do MD5 stuff, too
 *
 * Revision 1.1  1996/12/01 03:06:54  morgan
 * Initial revision
 *
 * Revision 1.1  1996/09/05 06:43:31  morgan
 * Initial revision
 *
 */

static void MD5Transform(uint32_t buf[4], uint32_t const in[16]);

#ifdef sun
#define HIGHFIRST
#endif

#ifndef HIGHFIRST
#define byteReverse(buf, len)	/* Nothing */
#else
/*
 * Note: this code is harmless on little-endian machines.
 */
static void byteReverse(unsigned char *buf, unsigned longs)
{
    uint32_t t;
    do {
	t = (uint32_t) ((unsigned) buf[3] << 8 | buf[2]) << 16 |
	    ((unsigned) buf[1] << 8 | buf[0]);
	*(uint32_t *) buf = t;
	buf += 4;
    } while (--longs);
}
#endif

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
void Ns_CtxMD5Init(Ns_CtxMD5 *ctx)
{
    ctx->buf[0] = 0x67452301U;
    ctx->buf[1] = 0xefcdab89U;
    ctx->buf[2] = 0x98badcfeU;
    ctx->buf[3] = 0x10325476U;

    ctx->bits[0] = 0;
    ctx->bits[1] = 0;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
void Ns_CtxMD5Update(Ns_CtxMD5 *ctx, unsigned const char *buf, unsigned len)
{
    uint32_t t;

    /* Update bitcount */

    t = ctx->bits[0];
    if ((ctx->bits[0] = t + ((uint32_t) len << 3)) < t)
	ctx->bits[1]++;		/* Carry from low to high */
    ctx->bits[1] += len >> 29;

    t = (t >> 3) & 0x3f;	/* Bytes already in shsInfo->data */

    /* Handle any leading odd-sized chunks */

    if (t) {
	unsigned char *p = (unsigned char *) ctx->in + t;

	t = 64 - t;
	if (len < t) {
	    memcpy(p, buf, len);
	    return;
	}
	memcpy(p, buf, t);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint32_t *) ctx->in);
	buf += t;
	len -= t;
    }
    /* Process data in 64-byte chunks */

    while (len >= 64) {
	memcpy(ctx->in, buf, 64);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint32_t *) ctx->in);
	buf += 64;
	len -= 64;
    }

    /* Handle any remaining bytes of data. */

    memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
void Ns_CtxMD5Final(Ns_CtxMD5 *ctx, unsigned char digest[16])
{
    unsigned count;
    unsigned char *p;

    /* Compute number of bytes mod 64 */
    count = (ctx->bits[0] >> 3) & 0x3F;

    /* Set the first char of padding to 0x80.  This is safe since there is
       always at least one byte free */
    p = ctx->in + count;
    *p++ = 0x80;

    /* Bytes of padding needed to make 64 bytes */
    count = 64 - 1 - count;

    /* Pad out to 56 mod 64 */
    if (count < 8) {
	/* Two lots of padding:  Pad the first block to 64 bytes */
	memset(p, 0, count);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint32_t *) ctx->in);

	/* Now fill the next block with 56 bytes */
	memset(ctx->in, 0, 56);
    } else {
	/* Pad block to 56 bytes */
	memset(p, 0, count - 8);
    }
    byteReverse(ctx->in, 14);

    /* Append length in bits and transform */
    ((uint32_t *) ctx->in)[14] = ctx->bits[0];
    ((uint32_t *) ctx->in)[15] = ctx->bits[1];

    MD5Transform(ctx->buf, (uint32_t *) ctx->in);
    byteReverse((unsigned char *) ctx->buf, 4);
    memcpy(digest, ctx->buf, 16);
    memset(ctx, 0, sizeof(ctx));	/* In case it's sensitive */
}

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f, w, x, y, z, data, s) \
	( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
static void MD5Transform(uint32_t buf[4], uint32_t const in[16])
{
    register uint32_t a, b, c, d;

    a = buf[0];
    b = buf[1];
    c = buf[2];
    d = buf[3];

    MD5STEP(F1, a, b, c, d,  in[0] + 0xd76aa478U,  7);
    MD5STEP(F1, d, a, b, c,  in[1] + 0xe8c7b756U, 12);
    MD5STEP(F1, c, d, a, b,  in[2] + 0x242070dbU, 17);
    MD5STEP(F1, b, c, d, a,  in[3] + 0xc1bdceeeU, 22);
    MD5STEP(F1, a, b, c, d,  in[4] + 0xf57c0fafU,  7);
    MD5STEP(F1, d, a, b, c,  in[5] + 0x4787c62aU, 12);
    MD5STEP(F1, c, d, a, b,  in[6] + 0xa8304613U, 17);
    MD5STEP(F1, b, c, d, a,  in[7] + 0xfd469501U, 22);
    MD5STEP(F1, a, b, c, d,  in[8] + 0x698098d8U,  7);
    MD5STEP(F1, d, a, b, c,  in[9] + 0x8b44f7afU, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1U, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7beU, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122U,  7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193U, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438eU, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821U, 22);

    MD5STEP(F2, a, b, c, d,  in[1] + 0xf61e2562U,  5);
    MD5STEP(F2, d, a, b, c,  in[6] + 0xc040b340U,  9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51U, 14);
    MD5STEP(F2, b, c, d, a,  in[0] + 0xe9b6c7aaU, 20);
    MD5STEP(F2, a, b, c, d,  in[5] + 0xd62f105dU,  5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453U,  9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681U, 14);
    MD5STEP(F2, b, c, d, a,  in[4] + 0xe7d3fbc8U, 20);
    MD5STEP(F2, a, b, c, d,  in[9] + 0x21e1cde6U,  5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6U,  9);
    MD5STEP(F2, c, d, a, b,  in[3] + 0xf4d50d87U, 14);
    MD5STEP(F2, b, c, d, a,  in[8] + 0x455a14edU, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905U,  5);
    MD5STEP(F2, d, a, b, c,  in[2] + 0xfcefa3f8U,  9);
    MD5STEP(F2, c, d, a, b,  in[7] + 0x676f02d9U, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8aU, 20);

    MD5STEP(F3, a, b, c, d,  in[5] + 0xfffa3942U,  4);
    MD5STEP(F3, d, a, b, c,  in[8] + 0x8771f681U, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122U, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380cU, 23);
    MD5STEP(F3, a, b, c, d,  in[1] + 0xa4beea44U,  4);
    MD5STEP(F3, d, a, b, c,  in[4] + 0x4bdecfa9U, 11);
    MD5STEP(F3, c, d, a, b,  in[7] + 0xf6bb4b60U, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70U, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6U,  4);
    MD5STEP(F3, d, a, b, c,  in[0] + 0xeaa127faU, 11);
    MD5STEP(F3, c, d, a, b,  in[3] + 0xd4ef3085U, 16);
    MD5STEP(F3, b, c, d, a,  in[6] + 0x04881d05U, 23);
    MD5STEP(F3, a, b, c, d,  in[9] + 0xd9d4d039U,  4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5U, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8U, 16);
    MD5STEP(F3, b, c, d, a,  in[2] + 0xc4ac5665U, 23);

    MD5STEP(F4, a, b, c, d,  in[0] + 0xf4292244U,  6);
    MD5STEP(F4, d, a, b, c,  in[7] + 0x432aff97U, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7U, 15);
    MD5STEP(F4, b, c, d, a,  in[5] + 0xfc93a039U, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3U,  6);
    MD5STEP(F4, d, a, b, c,  in[3] + 0x8f0ccc92U, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47dU, 15);
    MD5STEP(F4, b, c, d, a,  in[1] + 0x85845dd1U, 21);
    MD5STEP(F4, a, b, c, d,  in[8] + 0x6fa87e4fU,  6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0U, 10);
    MD5STEP(F4, c, d, a, b,  in[6] + 0xa3014314U, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1U, 21);
    MD5STEP(F4, a, b, c, d,  in[4] + 0xf7537e82U,  6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235U, 10);
    MD5STEP(F4, c, d, a, b,  in[2] + 0x2ad7d2bbU, 15);
    MD5STEP(F4, b, c, d, a,  in[9] + 0xeb86d391U, 21);

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

/*
 *----------------------------------------------------------------------
 *
 * MD5Cmd --
 *
 *      Returns a 32-character, hex-encoded string containing the MD5
 *      hash of the first argument.
 *
 * Results:
 *	NS_OK
 *
 * Side effects:
 *	Tcl result is set to a string value.
 *
 *----------------------------------------------------------------------
 */

int
NsTclMD5ObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                Tcl_Obj *CONST objv[])
{
    Ns_CtxMD5      ctx;
    unsigned char  digest[16];
    char           digestChars[33];
    char          *str;
    int            strLen;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    str = Tcl_GetStringFromObj(objv[1],&strLen);
    Ns_CtxMD5Init(&ctx);
    Ns_CtxMD5Update(&ctx, (unsigned char *) str, (unsigned int) strLen);
    Ns_CtxMD5Final(&ctx, digest);

    Ns_CtxString(digest, digestChars, 16);
    Tcl_AppendResult(interp, digestChars, NULL);

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclSetPrivilegesObjCmd --
 *
 *      Implements ns_setuser and ns_setgroup as ObjCommand.
 *
 * Results:
 *      Tcl result, 1 if sucessful, -1 on error
 *
 * Side effects:
 *      Error message will be output in the log file, not returned as Tcl result
 *
 *----------------------------------------------------------------------
 */

int
NsTclSetUserObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "user");
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_SetUser(Tcl_GetString(objv[1]))));
    return TCL_OK;
}

int
NsTclSetGroupObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "group");
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_SetGroup(Tcl_GetString(objv[1]))));
    return TCL_OK;
}

