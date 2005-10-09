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
    va_list         ap;
    Tcl_DString     ds;

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
NsTclRunOnceObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
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
    (void) Tcl_CreateHashEntry(global ? &runTable : &itPtr->servPtr->tcl.runTable,
                               script, &new);
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
        Ns_DStringVarAppend(&ds, conn->request->method, " ", conn->request->url,
                            ", PeerAddress: ", Ns_ConnPeer(conn), NULL);

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
 *	Implements ns_striphtml. 
 *
 * Results:
 *	Tcl result. 
 *
 * Side effects:
 *	See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclStripHtmlCmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    int   intag;     /* flag to see if are we inside a tag */
    int	  intspec;   /* flag to see if we are inside a special char */
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
 *	Implements ns_crypt as ObjCommand. 
 *
 * Results:
 *	Tcl result. 
 *
 * Side effects:
 *	See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclCryptObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char buf[NS_ENCRYPT_BUFSIZE];

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "key salt");
        return TCL_ERROR;
    }
    Tcl_SetResult(interp, Ns_Encrypt(Tcl_GetString(objv[1]), Tcl_GetString(objv[2]), buf), TCL_VOLATILE);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHrefsCmd --
 *
 *	Implements ns_hrefs. 
 *
 * Results:
 *	Tcl result. 
 *
 * Side effects:
 *	See docs. 
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
 *	Implements ns_uuencode as obj command.
 *
 * Results:
 *	Tcl result. 
 *
 * Side effects:
 *	See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclHTUUEncodeObjCmd(ClientData dummy, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    char *string;
    char *result;
    int   nbytes;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }
    string = Tcl_GetStringFromObj(objv[1], &nbytes);
    result = ns_malloc((size_t) 1 + (4 * nbytes) / 2);
    Ns_HtuuEncode((unsigned char *) string, (size_t)nbytes, result);
    Tcl_SetResult(interp, result, (Tcl_FreeProc *) ns_free);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HTUUDecodeObjcmd --
 *
 *	Implements ns_uudecode as obj command. 
 *
 * Results:
 *	Tcl result. 
 *
 * Side effects:
 *	See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclHTUUDecodeObjCmd(ClientData dummy, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
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
 *	Crash the server to test exception handling.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Server will segfault.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCrashCmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    char           *death;

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
 *      The SHA1 routines are borrowed from libmd, which contains the following header:
 *
 *          * sha.c - NIST Secure Hash Algorithm, FIPS PUB 180 and 180.1.
 *          * The algorithm is by spook(s) unknown at the U.S. National Security Agency.
 *          *
 *          * Written 2 September 1992, Peter C. Gutmann.
 *          * This implementation placed in the public domain.
 *          *
 *          * Modified 1 June 1993, Colin Plumb.
 *          * Modified for the new SHS based on Peter Gutmann's work,
 *          * 18 July 1994, Colin Plumb.
 *          *
 *          * Renamed to SHA and comments updated a bit 1 November 1995, Colin Plumb.
 *          * These modifications placed in the public domain.
 *          *
 *          * Comments to pgut1@cs.aukuni.ac.nz
 *          *
 *          * Hacked for use in libmd by Martin Hinner <mhi@penguin.cz>
 *
 *      This Tcl library was hacked by Jon Salz <jsalz@mit.edu>.
 *
 */

static char hexChars[] = "0123456789ABCDEF";

#ifndef __APPLE__
typedef unsigned int u_int32_t;
typedef unsigned char u_int8_t;
#endif

/*** FROM sha.h: ***/

/*
* Define to 1 for FIPS 180.1 version (with extra rotate in prescheduling),
* 0 for FIPS 180 version (with the mysterious "weakness" that the NSA
* isn't talking about).
*/
#define SHA_VERSION 1

#define SHA_BLOCKBYTES 64
#define SHA_BLOCKWORDS 16

#define SHA_HASHBYTES 20
#define SHA_HASHWORDS 5

/* SHA context. */
typedef struct SHAContext {
 unsigned int key[SHA_BLOCKWORDS];
 u_int32_t iv[SHA_HASHWORDS];
#ifdef HAVE64
 u_int64_t bytes;
#else
 u_int32_t bytesHi, bytesLo;
#endif
} SHA_CTX;

/*** END sha.h ***/

/*** FROM sha.c: ***/

/*
   Shuffle the bytes into big-endian order within words, as per the
   SHA spec.
 */
static void
shaByteSwap (u_int32_t * dest, u_int8_t const *src, unsigned int words)
{
  do
    {
      *dest++ = (u_int32_t) ((unsigned) src[0] << 8 | src[1]) << 16 |
	((unsigned) src[2] << 8 | src[3]);
      src += 4;
    }
  while (--words);
}

/* Initialize the SHA values */
static void
SHAInit (SHA_CTX * ctx)
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
SHATransform (struct SHAContext *sha)
{
  register u_int32_t A, B, C, D, E;
#if SHA_VERSION
  register u_int32_t t;
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
static void
SHAUpdate (SHA_CTX * ctx, const unsigned char *buf, unsigned int len)
{
  unsigned i;

/* Update bitcount */

#ifdef HAVE64
  i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
  ctx->bytes += len;
#else
  u_int32_t t = ctx->bytesLo;
  if ((ctx->bytesLo = t + len) < t)
    ctx->bytesHi++;		/* Carry from low to high */

  i = (unsigned) t % SHA_BLOCKBYTES;	/* Bytes already in ctx->key */
#endif

  /* i is always less than SHA_BLOCKBYTES. */
  if (SHA_BLOCKBYTES - i > len)
    {
      memcpy ((u_int8_t *) ctx->key + i, buf, len);
      return;
    }

  if (i)
    {				/* First chunk is an odd size */
      memcpy ((u_int8_t *) ctx->key + i, buf, SHA_BLOCKBYTES - i);
      shaByteSwap (ctx->key, (u_int8_t *) ctx->key, SHA_BLOCKWORDS);
      SHATransform (ctx);
      buf += SHA_BLOCKBYTES - i;
      len -= SHA_BLOCKBYTES - i;
    }

/* Process data in 64-byte chunks */
  while (len >= SHA_BLOCKBYTES)
    {
      shaByteSwap (ctx->key, buf, SHA_BLOCKWORDS);
      SHATransform (ctx);
      buf += SHA_BLOCKBYTES;
      len -= SHA_BLOCKBYTES;
    }

/* Handle any remaining bytes of data. */
  if (len)
    memcpy (ctx->key, buf, len);
}

/*
   * Final wrapup - pad to 64-byte boundary with the bit pattern
   * 1 0* (64-bit count of bits processed, MSB-first)
 */

static void
SHAFinal (unsigned char digest[20], SHA_CTX * ctx)
{
#if HAVE64
  unsigned i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
#else
  unsigned i = (unsigned) ctx->bytesLo % SHA_BLOCKBYTES;
#endif
  u_int8_t *p = (u_int8_t *) ctx->key + i;	/* First unused byte */
  u_int32_t t;

  /* Set the first char of padding to 0x80. There is always room. */
  *p++ = 0x80;

  /* Bytes of padding needed to make 64 bytes (0..63) */
  i = SHA_BLOCKBYTES - 1 - i;

  if (i < 8)
    {				/* Padding forces an extra block */
      memset (p, 0, i);
      shaByteSwap (ctx->key, (u_int8_t *) ctx->key, 16);
      SHATransform (ctx);
      p = (u_int8_t *) ctx->key;
      i = 64;
    }
  memset (p, 0, i - 8);
  shaByteSwap (ctx->key, (u_int8_t *) ctx->key, 14);

  /* Append length in bits and transform */
#if HAVE64
  ctx->key[14] = (u_int32_t) (ctx->bytes >> 29);
  ctx->key[15] = (u_int32_t) ctx->bytes << 3;
#else
  ctx->key[14] = ctx->bytesHi << 3 | ctx->bytesLo >> 29;
  ctx->key[15] = ctx->bytesLo << 3;
#endif
  SHATransform (ctx);

  memcpy (digest, ctx->iv, sizeof (digest));
  for (i = 0; i < SHA_HASHWORDS; i++)
    {
      t = ctx->iv[i];
      digest[i * 4 + 0] = (u_int8_t) (t >> 24);
      digest[i * 4 + 1] = (u_int8_t) (t >> 16);
      digest[i * 4 + 2] = (u_int8_t) (t >> 8);
      digest[i * 4 + 3] = (u_int8_t) t;
    }
  
 memset(ctx, 0, sizeof(ctx)); 			/* In case it's sensitive */
}

/*** END sha.c ***/



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
NsTclSHA1ObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    SHA_CTX        ctx;
    unsigned char  digest[20];
    char           digestChars[41];
    int            i;
    char          *str;
    int            strLen;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
	return TCL_ERROR;
    }

    str = Tcl_GetStringFromObj(objv[1],&strLen);
    SHAInit(&ctx);
    SHAUpdate(&ctx, (unsigned char *) str, (unsigned int) strLen);
    SHAFinal(digest, &ctx);

    for (i = 0; i < 20; ++i) {
	digestChars[i * 2] = hexChars[digest[i] >> 4];
	digestChars[i * 2 + 1] = hexChars[digest[i] & 0xF];
    }
    digestChars[40] = '\0';

    Tcl_AppendResult(interp, digestChars, NULL);
    
    return NS_OK;
}
