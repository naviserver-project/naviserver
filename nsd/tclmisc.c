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

/*
 * Local functions defined in this file
 */

static int WordEndsInSemi(const char *ip) NS_GNUC_NONNULL(1);
static void SHAByteSwap(uint32_t *dest, uint8_t const *src, unsigned int words)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void SHATransform(Ns_CtxSHA1 *sha) NS_GNUC_NONNULL(1);
static void MD5Transform(uint32_t buf[4], uint8_t const block[64]) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);



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
Ns_TclPrintfResult(Tcl_Interp *interp, const char *fmt, ...)
{
    va_list     ap;
    Tcl_DString ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(fmt != NULL);

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
NsTclRunOnceObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp       *itPtr = arg;
    const char           *script;
    int                   isNew, global = (int)NS_FALSE;
    static Tcl_HashTable  runTable;
    static int            initialized = (int)NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-global", Ns_ObjvBool,  &global, INT2PTR(NS_TRUE)},
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
    if (initialized == 0) {
        Tcl_InitHashTable(&runTable, TCL_STRING_KEYS);
        initialized = NS_TRUE;
    }
    (void) Tcl_CreateHashEntry((global != (int)NS_FALSE) ? &runTable :
                               &itPtr->servPtr->tcl.runTable, script, &isNew);
    Ns_MasterUnlock();

    if (isNew != 0) {
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

const char *
Ns_TclLogErrorInfo(Tcl_Interp *interp, const char *extraInfo)
{
    const NsInterp *itPtr = NsGetInterpData(interp);
    const char     *errorInfo, **logHeaders;
    Ns_DString      ds;

    if (extraInfo != NULL) {
        Tcl_AddObjErrorInfo(interp, extraInfo, -1);
    }
    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if (errorInfo == NULL) {
        errorInfo = "";
    }
    if (itPtr != NULL && itPtr->conn != NULL) {
        const Ns_Conn *conn = itPtr->conn;
        
        Ns_DStringInit(&ds);
        if (conn->request.method != NULL) {
            Ns_DStringVarAppend(&ds, conn->request.method, " ", NULL);
        }
        if (conn->request.url != NULL) {
            Ns_DStringVarAppend(&ds, conn->request.url, ", ", NULL);
        }
        Ns_DStringVarAppend(&ds, "PeerAddress: ", Ns_ConnPeer(conn), NULL);

        logHeaders = itPtr->servPtr->tcl.errorLogHeaders;
        if (logHeaders != NULL) {
	    const char  *const*hdr;

            for (hdr = logHeaders; *hdr != NULL; hdr++) {
	        const char *value = Ns_SetIGet(conn->headers, *hdr);

                if (value != NULL) {
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

const char *
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

const char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *UNUSED(conn))
{
    return Ns_TclLogErrorInfo(interp, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogDeprecated --
 *
 *      Report that a C-implmented Tcl command is deprecated.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Write log message.
 *
 *----------------------------------------------------------------------
 */

void
Ns_LogDeprecated(Tcl_Obj *CONST* objv, int objc, const char *alternative, const char *explanation)
{
    Tcl_DString ds;
    int i;

    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, "'", 1);
    for (i = 0; i < objc; i++) {
	const char *s;
	int len;

	s = Tcl_GetStringFromObj(objv[i], &len);
	Tcl_DStringAppend(&ds, s, len);
	Tcl_DStringAppend(&ds, " ", 1);
    }
    Tcl_DStringAppend(&ds, "' is deprecated. ", -1);
    if (alternative != NULL) {
	Tcl_DStringAppend(&ds, "Use '", -1);
	Tcl_DStringAppend(&ds, alternative, -1);
	Tcl_DStringAppend(&ds, "' instead. ", -1);
    }
    if (explanation != NULL) {
	Tcl_DStringAppend(&ds, explanation, -1);
    }
    Ns_Log(Notice, "%s", Tcl_DStringValue(&ds));
    Tcl_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetNamedVar --
 *
 *	Set a variable by denoted by a name.  Convenience routine for
 *	tcl-commands, when var names are passed in (e.g ns_http).
 *
 * Results:
 *	NS_TRUE on success, NS_FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SetNamedVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr)
{
    const Tcl_Obj *errPtr;

    Tcl_IncrRefCount(valPtr);
    errPtr = Tcl_ObjSetVar2(interp, varPtr, NULL, valPtr,
			       TCL_PARSE_PART1|TCL_LEAVE_ERR_MSG);
    Tcl_DecrRefCount(valPtr);
    return (errPtr != NULL ? NS_TRUE : NS_FALSE);
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
NsTclStripHtmlCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int argc, CONST84 char *argv[])
{
    int         intag;     /* flag to see if are we inside a tag */
    int         intspec;   /* flag to see if we are inside a special char */
    char       *inString;  /* copy of input string */
    char       *outPtr;    /* moving pointer to output string */
    const char *inPtr;     /* moving pointer to input string */

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

        } else if ((intag != 0) && (*inPtr == '>')) {
	    /* inside a tag that closes */
            intag = 0;

        } else if ((intspec != 0) && (*inPtr == ';')) {
	    /* inside a special character that closes */
            intspec = 0;

        } else if ((intag == 0) && (intspec == 0)) {
	    /* regular text */

            if (*inPtr == '&') {
		/* starting a new special character */
		intspec=WordEndsInSemi(inPtr);
	    }

            if (intspec == 0) {
		/* incr pointer only if we're not in something htmlish */
                *outPtr++ = *inPtr;
	    }
        }
        ++inPtr;
    }

    /* null-terminator */
    *outPtr = '\0';

    Tcl_SetObjResult(interp, Tcl_NewStringObj(inString, -1));

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
NsTclCryptObjCmd(ClientData UNUSED(arg), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
NsTclHrefsCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int argc, CONST84 char *argv[])
{
    char       *s, *e, *he, save;
    const char *p;

    if (argc != 2) {
        Tcl_AppendResult(interp, "wrong # args: should be \"",
                         argv[0], " html\"", (char *) NULL);
        return TCL_ERROR;
    }

    p = argv[1];
    while (((s = strchr(p, INTCHAR('<'))) != NULL) && ((e = strchr(s, INTCHAR('>'))) != NULL)) {
        ++s;
        *e = '\0';
        while (*s != '\0' && CHARTYPE(space, *s) != 0) {
            ++s;
        }
        if ((*s == 'a' || *s == 'A') && CHARTYPE(space, s[1]) != 0) {
            ++s;
            while (*s != '\0') {
                if (strncasecmp(s, "href", 4u) == 0) {
                    s += 4;
                    while (*s != '\0' && CHARTYPE(space, *s) != 0) {
                        ++s;
                    }
                    if (*s == '=') {
                        ++s;
                        while (*s != '\0' && CHARTYPE(space, *s) != 0) {
                            ++s;
                        }
                        he = NULL;
                        if (*s == '\'' || *s == '"') {
                            he = strchr(s+1, *s);
                            ++s;
                        }
                        if (he == NULL) {
                            assert(s != NULL);
                            he = s;
                            while (*he != '\0' && CHARTYPE(space, *he) == 0) {
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
                    while (*s != '\0' && (*s != '\'' || *s != '\"')) {
                        ++s;
                    }
                    continue;
                }
                if (*s != '\0') {
                    ++s;
                }
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
NsTclHTUUEncodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const unsigned char *bytes;
    char          *result;
    int            nbytes = 0;
    size_t         size;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    bytes = Tcl_GetByteArrayFromObj(objv[1], &nbytes);
    size = (size_t)nbytes;
    result = ns_malloc(1u + (4u * MAX(size,2u)) / 2u);
    (void)Ns_HtuuEncode(bytes, size, result);
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
NsTclHTUUDecodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            len;
    size_t         size;
    const char    *chars;
    unsigned char *decoded;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    chars = Tcl_GetStringFromObj(objv[1], &len);
    size = (size_t)len + 3u;
    decoded = (unsigned char *)ns_malloc(size);
    size = Ns_HtuuDecode(chars, decoded, size);
    decoded[size] = UCHAR('\0');
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(decoded, (int)size));
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
NsTclCrashCmd(ClientData UNUSED(clientData), Tcl_Interp *UNUSED(interp),
	      int UNUSED(argc), CONST char* UNUSED(argv[]))
{
    char *death;

    death = NULL;
    *death = 'x';

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
WordEndsInSemi(const char *ip)
{
    NS_NONNULL_ASSERT(ip != NULL);
    
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

/*
 * Define to 1 for FIPS 180.1 version (with extra rotate in prescheduling),
 * 0 for FIPS 180 version (with the mysterious "weakness" that the NSA
 * isn't talking about).
 */

#define SHA_VERSION 1

#define SHA_BLOCKBYTES 64u

/*
   Shuffle the bytes into big-endian order within words, as per the
   SHA spec.
 */


static void
SHAByteSwap(uint32_t *dest, uint8_t const *src, unsigned int words)
{
    do {
       *dest++ = (uint32_t) ((unsigned) src[0] << 8 | src[1]) << 16 |
	         ((unsigned) src[2] << 8 | src[3]);
       src += 4;
    } while (--words > 0u);
}

/* Initialize the SHA values */
void Ns_CtxSHAInit(Ns_CtxSHA1 * ctx)
{

    /* Set the h-vars to their initial values */
    ctx->iv[0] = 0x67452301u;
    ctx->iv[1] = 0xEFCDAB89u;
    ctx->iv[2] = 0x98BADCFEu;
    ctx->iv[3] = 0x10325476u;
    ctx->iv[4] = 0xC3D2E1F0u;

    /* Initialise bit count */
#if defined(HAVE_64BIT)
    ctx->bytes = 0u;
#else
    ctx->bytesHi = 0u;
    ctx->bytesLo = 0u;
#endif
}

/*
   The SHA f()-functions. The f1 and f3 functions can be optimized to
   save one boolean operation each - thanks to Rich Schroeppel,
   rcs@cs.arizona.edu for discovering this.
   The f3 function can be modified to use an addition to combine the
   two halves rather than OR, allowing more opportunity for using
   associativity in optimization. (Colin Plumb)
 */
#define f1(x,y,z) ( (z) ^ ((x) & ((y) ^ (z)) ) )	/* Rounds 0-19 */
#define f2(x,y,z) ( (x) ^ (y) ^ (z) )			/* Rounds 20-39 */
#define f3(x,y,z) ( ((x) & (y)) + ((z) & ((x) ^ (y)) ) )	/* Rounds 40-59 */
#define f4(x,y,z) ( (x) ^ (y) ^ (z) )			/* Rounds 60-79 */

/* The SHA Mysterious Constants. */
#define K2  (0x5A827999uL)	/* Rounds 0 -19 - floor(sqrt(2)  * 2^30) */
#define K3  (0x6ED9EBA1uL)	/* Rounds 20-39 - floor(sqrt(3)  * 2^30) */
#define K5  (0x8F1BBCDCuL)	/* Rounds 40-59 - floor(sqrt(5)  * 2^30) */
#define K10 (0xCA62C1D6uL)	/* Rounds 60-79 - floor(sqrt(10) * 2^30) */

/* 32-bit rotate left - kludged with shifts */
#define ROTL(n,X) ( ((X) << (n)) | ((X) >> (32-(n))) )

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

#define expandx(W,i) (t = W[(i)&15u] ^ W[((i)-14)&15u] ^ W[((i)-8)&15u] ^ W[((i)-3)&15u], \
			ROTL(1, t))
#define expand(W,i) (W[(i)&15u] = expandx(W,(i)))

#else /* Old FIPS 180 */

#define expandx(W,i) (W[(i)&15u] ^ W[((i)-14)&15u] ^ W[((i)-8)&15u] ^ W[((i)-3)&15u])
#define expand(W,i) (W[(i)&15u] ^= W[((i)-14)&15u] ^ W[((i)-8)&15u] ^ W[((i)-3)&15u])

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
    ( (e) += ROTL(5u,(a)) + f((b), (c), (d)) + (k) + (data), (b) = ROTL(30u, (b)) )
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

    NS_NONNULL_ASSERT(sha != NULL);

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
    subRound (E, A, B, C, D, f1, K2, expand (sha->key, 16u));
    subRound (D, E, A, B, C, f1, K2, expand (sha->key, 17u));
    subRound (C, D, E, A, B, f1, K2, expand (sha->key, 18u));
    subRound (B, C, D, E, A, f1, K2, expand (sha->key, 19u));

    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 20u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 21u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 22u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 23u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 24u));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 25u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 26u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 27u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 28u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 29u));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 30u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 31u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 32u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 33u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 34u));
    subRound (A, B, C, D, E, f2, K3, expand (sha->key, 35u));
    subRound (E, A, B, C, D, f2, K3, expand (sha->key, 36u));
    subRound (D, E, A, B, C, f2, K3, expand (sha->key, 37u));
    subRound (C, D, E, A, B, f2, K3, expand (sha->key, 38u));
    subRound (B, C, D, E, A, f2, K3, expand (sha->key, 39u));

    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 40u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 41u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 42u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 43u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 44u));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 45u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 46u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 47u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 48u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 49u));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 50u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 51u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 52u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 53u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 54u));
    subRound (A, B, C, D, E, f3, K5, expand (sha->key, 55u));
    subRound (E, A, B, C, D, f3, K5, expand (sha->key, 56u));
    subRound (D, E, A, B, C, f3, K5, expand (sha->key, 57u));
    subRound (C, D, E, A, B, f3, K5, expand (sha->key, 58u));
    subRound (B, C, D, E, A, f3, K5, expand (sha->key, 59u));

    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 60u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 61u));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 62u));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 63u));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 64u));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 65u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 66u));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 67u));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 68u));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 69u));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 70u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 71u));
    subRound (D, E, A, B, C, f4, K10, expand (sha->key, 72u));
    subRound (C, D, E, A, B, f4, K10, expand (sha->key, 73u));
    subRound (B, C, D, E, A, f4, K10, expand (sha->key, 74u));
    subRound (A, B, C, D, E, f4, K10, expand (sha->key, 75u));
    subRound (E, A, B, C, D, f4, K10, expand (sha->key, 76u));
    subRound (D, E, A, B, C, f4, K10, expandx (sha->key, 77u));
    subRound (C, D, E, A, B, f4, K10, expandx (sha->key, 78u));
    subRound (B, C, D, E, A, f4, K10, expandx (sha->key, 79u));

    /* Build message digest */
    sha->iv[0] += A;
    sha->iv[1] += B;
    sha->iv[2] += C;
    sha->iv[3] += D;
    sha->iv[4] += E;
}

/* Update SHA for a block of data. */
void Ns_CtxSHAUpdate(Ns_CtxSHA1 *ctx, const unsigned char *buf, size_t len)
{
    unsigned i;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    /* Update bitcount */

#if defined(HAVE_64BIT)
    i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
    ctx->bytes += len;
#else
    {
        uint32_t t = ctx->bytesLo;
        ctx->bytesLo = (uint32_t)(t + len);
        if (ctx->bytesLo < t) {
            ctx->bytesHi++;		/* Carry from low to high */
        }
        i = (unsigned) t % SHA_BLOCKBYTES;	/* Bytes already in ctx->key */
    }
#endif

    /* i is always less than SHA_BLOCKBYTES. */
    if (SHA_BLOCKBYTES - i > len) {
        memcpy(ctx->key + i, buf, len);
        return;
    }

    if (i != 0u) {				/* First chunk is an odd size */
        memcpy(ctx->key + i, buf, SHA_BLOCKBYTES - i);
        SHAByteSwap(ctx->key, (uint8_t const *) ctx->key, SHA_BLOCKWORDS);
        SHATransform(ctx);
        buf += SHA_BLOCKBYTES - i;
        len -= SHA_BLOCKBYTES - i;
    }

    /* Process data in 64-byte chunks */
    while (len >= SHA_BLOCKBYTES) {
        SHAByteSwap(ctx->key, buf, SHA_BLOCKWORDS);
        SHATransform(ctx);
        buf += SHA_BLOCKBYTES;
        len -= SHA_BLOCKBYTES;
    }

    /* Handle any remaining bytes of data. */
    if (len != 0u) {
       memcpy(ctx->key, buf, len);
    }
}

/*
   * Final wrapup - pad to 64-byte boundary with the bit pattern
   * 1 0* (64-bit count of bits processed, MSB-first)
 */

void Ns_CtxSHAFinal(Ns_CtxSHA1 *ctx, unsigned char digest[20])
{
#if defined(HAVE_64BIT)
    unsigned i = (unsigned) ctx->bytes % SHA_BLOCKBYTES;
#else
    unsigned i = (unsigned) ctx->bytesLo % SHA_BLOCKBYTES;
#endif
    uint8_t *p = (uint8_t *) ctx->key + i;	/* First unused byte */

    /* Set the first char of padding to 0x80. There is always room. */
    *p++ = (uint8_t)0x80u;

    /* Bytes of padding needed to make 64 bytes (0..63) */
    i = (SHA_BLOCKBYTES - 1u) - i;

    if (i < 8u) {				/* Padding forces an extra block */
        memset(p, 0, i);
        SHAByteSwap(ctx->key, (uint8_t const *) ctx->key, 16u);
        SHATransform(ctx);
        p = (uint8_t *) ctx->key;
        i = 64u;
    }
    memset(p, 0, i - 8u);
    SHAByteSwap(ctx->key, (uint8_t const *) ctx->key, 14u);

    /* Append length in bits and transform */
#if defined(HAVE_64BIT)
    ctx->key[14] = (uint32_t) (ctx->bytes >> 29);
    ctx->key[15] = (uint32_t) ctx->bytes << 3;
#else
    ctx->key[14] = ctx->bytesHi << 3 | ctx->bytesLo >> 29;
    ctx->key[15] = ctx->bytesLo << 3;
#endif
    SHATransform (ctx);

    /* 
     * The following memcpy() does not seem to be correct and is most likely
     * not needed, since the loop sets all elements of "digetst".
     */
    /*memcpy(digest, ctx->iv, sizeof(digest));*/

    for (i = 0u; i < SHA_HASHWORDS; i++) {
	uint32_t t = ctx->iv[i];

        digest[i * 4u     ] = (uint8_t) (t >> 24);
        digest[i * 4u + 1u] = (uint8_t) (t >> 16);
        digest[i * 4u + 2u] = (uint8_t) (t >> 8);
        digest[i * 4u + 3u] = (uint8_t) t;
    }

    memset(ctx, 0, sizeof(Ns_CtxSHA1)); 			/* In case it's sensitive */
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_HexString --
 *
 *      Transform binary data to hex. The provided buffer must be
 *      at least size*2 + 1 bytes long.
 *
 * Results:
 *      buffer
 *
 * Side effects:
 *      Updates passed-in buffer (2nd argument).
 *
 *----------------------------------------------------------------------
 */
char *Ns_HexString(const unsigned char *digest, char *buf, int size, bool isUpper)
{
    int i;
    static const char hexCharsUpper[] = "0123456789ABCDEF";
    static const char hexCharsLower[] = "0123456789abcdef";
    
    if (isUpper) {
        for (i = 0; i < size; ++i) {
            buf[i * 2] = hexCharsUpper[digest[i] >> 4];
            buf[i * 2 + 1] = hexCharsUpper[digest[i] & 0xFu];
        }
    } else {
        for (i = 0; i < size; ++i) {
            buf[i * 2] = hexCharsLower[digest[i] >> 4];
            buf[i * 2 + 1] = hexCharsLower[digest[i] & 0xFu];
        }
    }
    buf[size * 2] = '\0';

    return buf;
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
NsTclSHA1ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_CtxSHA1     ctx;
    unsigned char  digest[20];
    char           digestChars[41];
    const char    *str;
    int            length;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }

    str = Ns_GetBinaryString(objv[1], &length);

    Ns_CtxSHAInit(&ctx);
    Ns_CtxSHAUpdate(&ctx, (const unsigned char *) str, (size_t) length);
    Ns_CtxSHAFinal(&ctx, digest);

    Ns_HexString(digest, digestChars, 20, NS_TRUE);
    Tcl_AppendResult(interp, digestChars, NULL);

    return TCL_OK;
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
NsTclFileStatObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    struct stat st;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "file ?varname?");
        return TCL_ERROR;
    }
    if (stat(Tcl_GetString(objv[1]), &st) != 0) {
        Tcl_SetResult(interp, "0", TCL_STATIC);
        return TCL_OK;
    }
    if (objc > 2) {
        const char *name = Tcl_GetString(objv[2]);
        
        (void)Tcl_SetVar2Ex(interp, name, "dev",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_dev), 0);
        (void)Tcl_SetVar2Ex(interp, name, "ino",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_ino), 0);
        (void)Tcl_SetVar2Ex(interp, name, "nlink", Tcl_NewWideIntObj((Tcl_WideInt)st.st_nlink), 0);
        (void)Tcl_SetVar2Ex(interp, name, "uid",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_uid), 0);
        (void)Tcl_SetVar2Ex(interp, name, "gid",   Tcl_NewWideIntObj((Tcl_WideInt)st.st_gid), 0);
        (void)Tcl_SetVar2Ex(interp, name, "size",  Tcl_NewWideIntObj((Tcl_WideInt)st.st_size), 0);
        (void)Tcl_SetVar2Ex(interp, name, "atime", Tcl_NewWideIntObj((Tcl_WideInt)st.st_atime), 0);
        (void)Tcl_SetVar2Ex(interp, name, "ctime", Tcl_NewWideIntObj((Tcl_WideInt)st.st_ctime), 0);
        (void)Tcl_SetVar2Ex(interp, name, "mtime", Tcl_NewWideIntObj((Tcl_WideInt)st.st_mtime), 0);
        (void)Tcl_SetVar2Ex(interp, name, "mode",  Tcl_NewIntObj(st.st_mode), 0);
        (void)Tcl_SetVar2Ex(interp, name, "type",  Tcl_NewStringObj(
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
    return TCL_OK;
}

/*
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
 */

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
    do {
	uint32_t t;

	t = (uint32_t) 
	    ((unsigned) buf[3] << 8 | buf[2]) << 16 |
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
    ctx->buf[0] = 0x67452301u;
    ctx->buf[1] = 0xefcdab89u;
    ctx->buf[2] = 0x98badcfeU;
    ctx->buf[3] = 0x10325476u;

    ctx->bits[0] = 0u;
    ctx->bits[1] = 0u;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
void Ns_CtxMD5Update(Ns_CtxMD5 *ctx, unsigned const char *buf, size_t len)
{
    uint32_t t;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    /* Update bitcount */

    t = ctx->bits[0];
    ctx->bits[0] = t + ((uint32_t) len << 3);
    if (ctx->bits[0] < t) {
	ctx->bits[1]++;		/* Carry from low to high */
    }
    ctx->bits[1] += (uint32_t)(len >> 29);

    t = (t >> 3) & 0x3Fu;	/* Bytes already in shsInfo->data */

    /* Handle any leading odd-sized chunks */

    if (t != 0u) {
	unsigned char *p = ctx->in + t;

	t = 64u - t;
	if (len < t) {
	    memcpy(p, buf, len);
	    return;
	}
	memcpy(p, buf, t);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint8_t *) ctx->in);
	buf += t;
	len -= t;
    }
    /* Process data in 64-byte chunks */

    while (len >= 64u) {
	memcpy(ctx->in, buf, 64u);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint8_t *) ctx->in);
	buf += 64;
	len -= 64u;
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
    uint8_t  *p;
    uint32_t *words;

    NS_NONNULL_ASSERT(ctx != NULL);
    NS_NONNULL_ASSERT(digest != NULL);

    words = (uint32_t *)ctx->in;
    
    /* Compute number of bytes mod 64 */
    count = (ctx->bits[0] >> 3) & 0x3Fu;

    /* 
     * Set the first char of padding to 0x80.  This is safe since there is
     * always at least one byte free
     */
    p = ctx->in + count;
    *p++ = (uint8_t)0x80u;
    
    /* Bytes of padding needed to make 64 bytes */
    count = (64u - 1u) - count;

    /* Pad out to 56 mod 64 */
    if (count < 8u) {
	/* Two lots of padding:  Pad the first block to 64 bytes */
	memset(p, 0, count);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (uint8_t *) ctx->in);

	/* Now fill the next block with 56 bytes */
	memset(ctx->in, 0, 56u);
    } else {
	/* Pad block to 56 bytes */
	memset(p, 0, count - 8u);
    }
    byteReverse(ctx->in, 14);

    /* Append length in bits and transform */
    words[14] = ctx->bits[0];
    words[15] = ctx->bits[1];

    MD5Transform(ctx->buf, (uint8_t *) ctx->in);
    byteReverse((unsigned char *) ctx->buf, 4);
    memcpy(digest, ctx->buf, 16u);
    /*
     * This memset should not be needed, since this is performed at the end of
     * the operation. In case, it would be needed, it should be necessary at
     * the initilization of the structure.
     *
     *     memset(ctx, 0, sizeof(Ns_CtxMD5));
     */
}

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define F2(x, y, z) (F1((z), (x), (y)))
#define F3(x, y, z) ((x) ^ (y) ^ (z))
#define F4(x, y, z) ((y) ^ ((x) | ~(z)))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f, w, x, y, z, data, s) \
    ( (w) += f((x), (y), (z)) + (data),  (w) = (w)<<(s) | (w)>>(32-(s)),  (w) += (x) )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to reflect
 * the addition of 16 32-bit words (64 bytes) of new data.  MD5Update blocks the
 * data and converts bytes into longwords for this routine.
 */
static void MD5Transform(uint32_t buf[4], uint8_t const block[64])
{
    register uint32_t a, b, c, d;

#ifndef HIGHFIRST
    const uint32_t *in = (const uint32_t *)block;
#else
    uint32_t in[16];
    
    memcpy(in, block, sizeof(in));

    for (a = 0; a < 16; a++) {
        in[a] = (uint32_t)(
                            (uint32_t)(block[a * 4 + 0]) |
                            (uint32_t)(block[a * 4 + 1]) <<  8 |
                            (uint32_t)(block[a * 4 + 2]) << 16 |
                            (uint32_t)(block[a * 4 + 3]) << 24);
    }
#endif

    NS_NONNULL_ASSERT(buf != NULL);
    NS_NONNULL_ASSERT(block != NULL);
    
    a = buf[0];
    b = buf[1];
    c = buf[2];
    d = buf[3];

    MD5STEP(F1, a, b, c, d,  in[0] + 0xd76aa478u,  7);
    MD5STEP(F1, d, a, b, c,  in[1] + 0xe8c7b756u, 12);
    MD5STEP(F1, c, d, a, b,  in[2] + 0x242070dbU, 17);
    MD5STEP(F1, b, c, d, a,  in[3] + 0xc1bdceeeU, 22);
    MD5STEP(F1, a, b, c, d,  in[4] + 0xf57c0fafU,  7);
    MD5STEP(F1, d, a, b, c,  in[5] + 0x4787c62aU, 12);
    MD5STEP(F1, c, d, a, b,  in[6] + 0xa8304613u, 17);
    MD5STEP(F1, b, c, d, a,  in[7] + 0xfd469501u, 22);
    MD5STEP(F1, a, b, c, d,  in[8] + 0x698098d8u,  7);
    MD5STEP(F1, d, a, b, c,  in[9] + 0x8b44f7afU, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1u, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7beU, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122u,  7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193u, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438eU, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821u, 22);

    MD5STEP(F2, a, b, c, d,  in[1] + 0xf61e2562u,  5);
    MD5STEP(F2, d, a, b, c,  in[6] + 0xc040b340u,  9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51u, 14);
    MD5STEP(F2, b, c, d, a,  in[0] + 0xe9b6c7aaU, 20);
    MD5STEP(F2, a, b, c, d,  in[5] + 0xd62f105dU,  5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453u,  9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681u, 14);
    MD5STEP(F2, b, c, d, a,  in[4] + 0xe7d3fbc8u, 20);
    MD5STEP(F2, a, b, c, d,  in[9] + 0x21e1cde6u,  5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6u,  9);
    MD5STEP(F2, c, d, a, b,  in[3] + 0xf4d50d87u, 14);
    MD5STEP(F2, b, c, d, a,  in[8] + 0x455a14edU, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905u,  5);
    MD5STEP(F2, d, a, b, c,  in[2] + 0xfcefa3f8u,  9);
    MD5STEP(F2, c, d, a, b,  in[7] + 0x676f02d9u, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8aU, 20);

    MD5STEP(F3, a, b, c, d,  in[5] + 0xfffa3942u,  4);
    MD5STEP(F3, d, a, b, c,  in[8] + 0x8771f681u, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122u, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380cU, 23);
    MD5STEP(F3, a, b, c, d,  in[1] + 0xa4beea44u,  4);
    MD5STEP(F3, d, a, b, c,  in[4] + 0x4bdecfa9u, 11);
    MD5STEP(F3, c, d, a, b,  in[7] + 0xf6bb4b60u, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70u, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6u,  4);
    MD5STEP(F3, d, a, b, c,  in[0] + 0xeaa127faU, 11);
    MD5STEP(F3, c, d, a, b,  in[3] + 0xd4ef3085u, 16);
    MD5STEP(F3, b, c, d, a,  in[6] + 0x04881d05u, 23);
    MD5STEP(F3, a, b, c, d,  in[9] + 0xd9d4d039u,  4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5u, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8u, 16);
    MD5STEP(F3, b, c, d, a,  in[2] + 0xc4ac5665u, 23);

    MD5STEP(F4, a, b, c, d,  in[0] + 0xf4292244u,  6);
    MD5STEP(F4, d, a, b, c,  in[7] + 0x432aff97u, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7u, 15);
    MD5STEP(F4, b, c, d, a,  in[5] + 0xfc93a039u, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3u,  6);
    MD5STEP(F4, d, a, b, c,  in[3] + 0x8f0ccc92u, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47dU, 15);
    MD5STEP(F4, b, c, d, a,  in[1] + 0x85845dd1u, 21);
    MD5STEP(F4, a, b, c, d,  in[8] + 0x6fa87e4fU,  6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0u, 10);
    MD5STEP(F4, c, d, a, b,  in[6] + 0xa3014314u, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1u, 21);
    MD5STEP(F4, a, b, c, d,  in[4] + 0xf7537e82u,  6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235u, 10);
    MD5STEP(F4, c, d, a, b,  in[2] + 0x2ad7d2bbU, 15);
    MD5STEP(F4, b, c, d, a,  in[9] + 0xeb86d391u, 21);

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
NsTclMD5ObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_CtxMD5      ctx;
    unsigned char  digest[16];
    char           digestChars[33];
    const char    *str;
    int            length;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }
    
    str = Ns_GetBinaryString(objv[1], &length);
        
    Ns_CtxMD5Init(&ctx);
    Ns_CtxMD5Update(&ctx, (const unsigned char *) str, (size_t)length);
    Ns_CtxMD5Final(&ctx, digest);

    Ns_HexString(digest, digestChars, 16, NS_TRUE);
    Tcl_AppendResult(interp, digestChars, NULL);

    return TCL_OK;
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
NsTclSetUserObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "user");
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_SetUser(Tcl_GetString(objv[1]))));
    return TCL_OK;
}

int
NsTclSetGroupObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "group");
        return TCL_ERROR;
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_SetGroup(Tcl_GetString(objv[1]))));
    return TCL_OK;
}

#ifndef _WIN32

/*
 *----------------------------------------------------------------------
 *
 * GetLimitObj --
 *
 *      Get single resource limit in form of a Tcl_Obj
 *
 * Results:
 *	Tcl_Obj
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
GetLimitObj(Tcl_WideInt value)
{
    Tcl_Obj *obj;
    
    if (value == RLIM_INFINITY) {
        obj = Tcl_NewStringObj("unlimited", -1);
    } else {
        obj = Tcl_NewWideIntObj(value);
    }
    return obj;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * NsTclRlimitObjCmd --
 *
 *      Get or Set resource limit in the operating system.
 *
 * Results:
 *	pair of actual value and maximum value
 *
 * Side effects:
 *	Change resource limiat with called with a value.
 *
 *----------------------------------------------------------------------
 */
int
NsTclRlimitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
#ifndef _WIN32
    int            opt, result = TCL_OK, rc;
    struct rlimit  rlimit;

    static const char *const opts[] = {
        "coresize",
        "datasize",
        "files",
        "filesize",        
        "vmsize",
        NULL
    };
    static int resource[] = {
        RLIMIT_CORE,
        RLIMIT_DATA,
        RLIMIT_NOFILE,
        RLIMIT_FSIZE,
        RLIMIT_AS
    };
    enum {
        CCoresizeIdx,
        CDatasizeIdx,
        CFIlesizeIdx,        
        CFilesIdx,
        CVmsizeIdx,
        
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, 
                            "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    if (objc == 2) {
        rc = getrlimit(resource[opt], &rlimit);
        if (rc == -1) {
            Ns_TclPrintfResult(interp, "getrlimit returned error");
            result = TCL_ERROR;
        }
    } else if (objc == 3) {
        Tcl_WideInt value;

        result = Tcl_GetWideIntFromObj(interp, objv[2], &value);
        if (result != TCL_OK) {
            char *valueString = Tcl_GetString(objv[2]);
            if (strcmp(valueString, "unlimited") == 0) {
                value = RLIM_INFINITY;
                result = TCL_OK;
            }
        }
        if (result == TCL_OK) {
            rc = getrlimit(resource[opt], &rlimit);
            if (rc > -1) {
                rlimit.rlim_cur = value;
                rc = setrlimit(resource[opt], &rlimit);
            }
            if (rc == -1) {
                Ns_TclPrintfResult(interp, "could not set limit");
                result = TCL_ERROR;                
            }
        }
    } else {
        Ns_TclPrintfResult(interp, "wrong # of arguments");
        result = TCL_ERROR;                
    }

    if (result == TCL_OK) {
        Tcl_Obj *listPtr = Tcl_NewListObj(2, NULL);
        
        Tcl_ListObjAppendElement(interp, listPtr, GetLimitObj(rlimit.rlim_cur));
        Tcl_ListObjAppendElement(interp, listPtr, GetLimitObj(rlimit.rlim_max));
        Tcl_SetObjResult(interp, listPtr);
        result = TCL_OK;
    }
    
    return result;
#else
    return TCL_OK;
#endif
}



/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
