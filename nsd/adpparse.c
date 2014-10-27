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
 * adpparse.c --
 *
 *      ADP parser.
 */

#include "nsd.h"

#define SERV_STREAM 0x01U
#define SERV_RUNAT  0x02U
#define SERV_NOTTCL 0x04U

#define TAG_ADP     1
#define TAG_PROC    2
#define TAG_SCRIPT  3

#define APPEND      "ns_adp_append "
#define APPEND_LEN  (sizeof(APPEND)-1)

#define LENSZ       ((int)(sizeof(int)))

/*
 * The following structure maintains proc and adp registered tags.
 * String bytes directly follow the Tag struct in the same allocated
 * block.
 */

typedef struct Tag {
    int            type;   /* Type of tag, ADP or proc. */
    char          *tag;    /* The name of the tag (e.g., "mytag") */
    char          *endtag; /* The closing tag or null (e.g., "/mytag")*/
    char          *string; /* Proc (e.g., "ns_adp_netscape") or ADP string. */
} Tag;

/*
 * The following strucutre maintains state while parsing an ADP block.
 */

typedef struct Parse {
    AdpCode       *codePtr; /* Pointer to compiled AdpCode struct. */
    int            line;    /* Current line number while parsing. */
    Tcl_DString    lens;    /* Length of text or script block. */
    Tcl_DString    lines;   /* Line number of block for debug messages. */
} Parse;

/*
 * Local functions defined in this file
 */

static void AppendBlock(Parse *parsePtr, const char *s, char *e, char type, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void AppendTag(Parse *parsePtr, const Tag *tagPtr, char *as, const char *ae, char *se, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static int RegisterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, int type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AppendLengths(AdpCode *codePtr, const int *length, const int *line)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void GetTag(Tcl_DString *dsPtr, char *s, const char *e, char **aPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static char *GetScript(const char *tag, char *a, char *e, unsigned int *streamFlagPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4); 

static void ParseAtts(char *s, const char *e, unsigned int *flagsPtr, Tcl_DString *attsPtr, int atts)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpRegisterAdpObjCmd, NsTclAdpRegisterProcObjCmd,
 * NsTclAdpRegisterScriptObjCmd --
 *
 *      Register an proc, script, are ADP string tag.
 *
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      An ADP tag may be added to the hashtable.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAdpRegisterAdpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return RegisterObjCmd(arg, interp, objc, objv, TAG_ADP);
}

int
NsTclAdpRegisterTagObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_LogDeprecated(objv, 1, "ns_adp_registeradp", NULL);
    return RegisterObjCmd(arg, interp, objc, objv, TAG_ADP);
}

int
NsTclAdpRegisterProcObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return RegisterObjCmd(arg, interp, objc, objv, TAG_PROC);
}

int
NsTclAdpRegisterScriptObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return RegisterObjCmd(arg, interp, objc, objv, TAG_SCRIPT);
}

int
NsTclAdpRegisterAdptagObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_LogDeprecated(objv, 1, "ns_adp_registerscript", NULL);
    return RegisterObjCmd(arg, interp, objc, objv, TAG_SCRIPT);
}

static int
RegisterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, int type)
{
    NsInterp       *itPtr = arg;
    NsServer       *servPtr;
    char           *string, *end, *tag;
    Tcl_HashEntry  *hPtr;
    int             isNew, slen, elen, tlen;
    Tcl_DString     tbuf;
    Tag            *tagPtr;

    assert(arg != NULL);
    assert(interp != NULL);

    servPtr = itPtr->servPtr;

    if (objc != 4 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "tag ?endtag? [adp|proc]");
        return TCL_ERROR;
    }

    string = Tcl_GetStringFromObj(objv[objc-1], &slen);
    ++slen;
    if (objc == 3) {
        end = NULL;
        elen = 0;
    } else {
        end = Tcl_GetStringFromObj(objv[2], &elen);
        ++elen;
    }

    tagPtr = ns_malloc(sizeof(Tag) + slen + elen);
    tagPtr->type = type;
    tagPtr->string = (char *) tagPtr + sizeof(Tag);
    memcpy(tagPtr->string, string, (size_t) slen);
    Tcl_UtfToLower(tagPtr->string);
    if (end == NULL) {
        tagPtr->endtag = NULL;
    } else {
        tagPtr->endtag = tagPtr->string + slen;
        memcpy(tagPtr->endtag, end, (size_t) elen);
        Tcl_UtfToLower(tagPtr->endtag);
    }

    Tcl_DStringInit(&tbuf);
    tag = Tcl_GetStringFromObj(objv[1], &tlen);
    Tcl_UtfToLower(Tcl_DStringAppend(&tbuf, tag, tlen));
    Ns_RWLockWrLock(&servPtr->adp.taglock);
    hPtr = Tcl_CreateHashEntry(&servPtr->adp.tags, tbuf.string, &isNew);
    if (!isNew) {
        ns_free(Tcl_GetHashValue(hPtr));
    }
    Tcl_SetHashValue(hPtr, tagPtr);
    tagPtr->tag = Tcl_GetHashKey(&servPtr->adp.tags, hPtr);
    Ns_RWLockUnlock(&servPtr->adp.taglock);
    Tcl_DStringFree(&tbuf);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpParse --
 *
 *      Parse a string of ADP text/script.  Parsing is done in a single,
 *      top to bottom pass, looking for the following four types of
 *      embedded script sequences:
 *
 *      1. <% tcl script %>
 *      2. <script runat=server language=tcl> tcl script </script>
 *      3. <registered-tag arg=val arg=val>
 *      4. <registered-start-tag arg=val arg=val> text </registered-end-tag>
 *
 *      Nested sequences are handled for each case, for example:
 *
 *      Text <% ns_adp_eval {<% ... %>} %> text ...
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Given AdpCode structure is initialized and filled in with copy
 *      of parsed ADP.
 *
 *----------------------------------------------------------------------
 */

void
NsAdpParse(AdpCode *codePtr, NsServer *servPtr, char *adp, 
	   unsigned int flags, CONST char* file)
{
    int             level, streamDone;
    unsigned int    streamFlag;
    Tag            *tagPtr = NULL;
    char           *script = "", *s, *e, *n;
    char           *a, *as = "", *ae = "", *text;
    Tcl_DString     tag;
    Tcl_HashEntry  *hPtr;
    enum {
        TagNext,
        TagScript,
        TagReg
    } state;
    Parse parse;

    /*
     * Initialize the code and parse structures.
     */

    Tcl_DStringInit(&codePtr->text);
    codePtr->nscripts = codePtr->nblocks = 0;
    parse.codePtr = codePtr;
    parse.line = 0;

    /*
     * Special case when we evalutating Tcl file, we just wrap it as
     * Tcl proc and save in ADP block with cache enabled or
     * just execute the Tcl code in case of cache disabled
     */

    if (flags & ADP_TCLFILE) {
        int size;

        if (!(flags & ADP_CACHE)) {
            Tcl_DStringAppend(&codePtr->text, adp, -1);
        } else {
            Ns_DStringPrintf(&codePtr->text,
                "ns_adp_append {<%%"
                "if {[info proc adp:%s] == {}} {"
                "  proc adp:%s {} { uplevel [for {", file, file);
            Tcl_DStringAppend(&codePtr->text, adp, -1);
            Ns_DStringPrintf(&codePtr->text, "} {0} {} {}]}}\nadp:%s %%>}", file);
        }
        codePtr->nblocks = codePtr->nscripts = 1;
        size = -codePtr->text.length;
        AppendLengths(codePtr, &size, &parse.line);

        return;
    }

    Tcl_DStringInit(&tag);
    Tcl_DStringInit(&parse.lens);
    Tcl_DStringInit(&parse.lines);

    /*
     * Parse ADP one tag at a time.
     */

    text = adp;
    streamDone = 0;
    streamFlag = 0U;
    level = 0;
    state = TagNext;
    Ns_RWLockRdLock(&servPtr->adp.taglock);

    while ((s = strchr(adp, '<')) && (e = strchr(s, '>'))) {
        /*
         * Process the tag depending on the current state.
         */

        switch (state) {
        case TagNext:
            /*
             * Look for a <% ... %> sequence.
             */

            if (s[1] == '%' && s[2] != '>') {   /* NB: Avoid <%>. */
                /*
                 * Find the cooresponding %> beyond any additional
                 * nested <% ... %> sequences.
                 */

                e = strstr(e - 1, "%>");
                n = s + 2;
                while (e != NULL && (n = strstr(n, "<%")) != NULL && n < e) {
                    n = n + 2;
                    e = strstr(e + 2, "%>");
                }
                if (e == NULL) {
                    /*
                     * No matching %> found.  Append text and invalid
                     * opening <% before searching for next ADP tag.
                     */

                    AppendBlock(&parse, text, s + 2, 't', flags);
                    text = s + 2; /* NB: Next text after invalid opening <%. */
                } else {
                    /*
                     * Append text block followed by script block unless
                     * in safe mode which suppresses in-line scripts and
                     * continue looking for next ADP tag.
                     */

		    if (s > text) {
			AppendBlock(&parse, text, s, 't', flags);
		    }
                    if (!(flags & ADP_SAFE)) {
                        if (s[2] != '=') {
                            AppendBlock(&parse, s + 2, e, 's', flags);
                        } else {
                            AppendBlock(&parse, s + 3, e, 'S', flags);
                        }
                    }
                    text = e + 2; /* NB: Next text after valid closing %>. */
                }
                s = text - 1; /* NB: Will incr +1, past text, below. */
            } else {
                /*
                 * Check for <script> tags or registered tags.
                 */

                GetTag(&tag, s, e, &a);
                script = GetScript(tag.string, a, e, &streamFlag);
                if (script != NULL) {
                    /*
                     * Append text and begin looking for closing </script> tag.
                     */

                    AppendBlock(&parse, text, s, 't', flags);
                    state = TagScript;
                    level = 1;
                } else {
                    hPtr = Tcl_FindHashEntry(&servPtr->adp.tags, tag.string);
                    if (hPtr != NULL) {
                        /*
                         * Append text and the registered tag content
                         * if the tag does not require a closing tag.
                         * Otherwise, save the tag attribute offsets
                         * and begin looking for required closing tag.
                         */

                        if (s > text) {
			    AppendBlock(&parse, text, s, 't', flags);
			}
                        tagPtr = Tcl_GetHashValue(hPtr);
                        if (tagPtr->endtag == NULL) {
                            AppendTag(&parse, tagPtr, a, e, NULL, flags);
                            text = e + 1;
                        } else {
                            as = a;
                            ae = e;
                            level = 1;
                            state = TagReg;
                        }
                    }
                }
            }
            break;

        case TagScript:
            /*
             * Look for corresponding closing </script> tag, handling
             * possible nesting of other <script> tags.
             */

            GetTag(&tag, s, e, NULL);
            if (STREQ(tag.string, "script")) {
                ++level;
            } else if (STREQ(tag.string, "/script")) {
                --level;
                if (level == 0) {
                    /*
                     * Found closing tag.  If not in safe mode,
                     * enable streaming if requested and appending
                     * the embedded script and then begin looking
                     * for next ADP tag.
                     */

                    if (!(flags & ADP_SAFE)) {
                        if (streamFlag && !streamDone) {
			    static char *buffer = "ns_adp_ctl stream on";
			    char *end = buffer + strlen(buffer);

                            AppendBlock(&parse, buffer, end, 's', flags);
                            streamDone = 1;
                        }
                        AppendBlock(&parse, script, s, 's', flags);
                    }
                    text = e + 1;
                    state = TagNext;
                }
            }
            break;

        case TagReg:
            /*
             * Looking for cooresponding closing tag for a registered
             * tag, handling possible nesting of the same tag.
             */

            GetTag(&tag, s, e, NULL);
            if (STREQ(tag.string, tagPtr->tag)) {
                ++level;
            } else if (STREQ(tag.string, tagPtr->endtag)) {
                --level;
                if (level == 0) {
                    /*
                     * Found closing tag. Append tag content and
                     * being looking for next ADP tag.
                     */

                    AppendTag(&parse, tagPtr, as, ae, s, flags);
                    text = e + 1;
                    state = TagNext;
                }
            }
            break;
        }

        /*
         * Skip to next possible ADP tag location.
         */

        adp = s + 1;
    }
    Ns_RWLockUnlock(&servPtr->adp.taglock);

    /*
     * Append the remaining text block
     */
    { 
	size_t len = strlen(text);
	if (len > 0U) {
	    AppendBlock(&parse, text, text + len, 't', flags);
	}
    }
    /*
     * If requested, collapse blocks to a single Tcl script and
     * and complete the parse code structure.
     */

    if (flags & ADP_SINGLE) {
        int line = 0, len = -codePtr->text.length;
        codePtr->nscripts = codePtr->nblocks = 1;
        AppendLengths(codePtr, &len, &line);
    } else {
        AppendLengths(codePtr, (int *) parse.lens.string,
                      (int *) parse.lines.string);
    }

    Tcl_DStringFree(&parse.lens);
    Tcl_DStringFree(&parse.lines);
    Tcl_DStringFree(&tag);
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpFreeCode --
 *
 *      Free internal AdpCode storage.
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
NsAdpFreeCode(AdpCode *codePtr)
{
    Tcl_DStringFree(&codePtr->text);
    codePtr->nblocks = codePtr->nscripts = 0;
    codePtr->len = codePtr->line = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * AppendBlock --
 *
 *      Add a text or script block to the output buffer.
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
AppendBlock(Parse *parsePtr, const char *s, char *e, char type, unsigned int flags)
{
    AdpCode   *codePtr;
    ptrdiff_t  len;

    assert(parsePtr != NULL);
    assert(s != NULL);
    assert(e != NULL);
    assert(s <= e);

    if (s == e) {
	/* false alarm */
	return;
    }

    codePtr = parsePtr->codePtr;

    if (flags & ADP_SINGLE) {
        char     save;

        switch (type) {
        case 'S':
            Tcl_DStringAppend(&codePtr->text, APPEND, (int)APPEND_LEN);
            Tcl_DStringAppend(&codePtr->text, s, (int)(e - s));
            break;

        case 't':
            save = *e;
            *e = '\0';
            Tcl_DStringAppend(&codePtr->text, APPEND, (int)APPEND_LEN);
            Tcl_DStringAppendElement(&codePtr->text, s);
            *e = save;
            break;

        default:
	  Tcl_DStringAppend(&codePtr->text, s, (int)(e - s));

        }
        Tcl_DStringAppend(&codePtr->text, "\n", 1);

    } else {

        ++codePtr->nblocks;
        len = e - s;
        if (type == 'S') {
            len += APPEND_LEN;
            Tcl_DStringAppend(&codePtr->text, APPEND, (int)APPEND_LEN);
        }
        Tcl_DStringAppend(&codePtr->text, s, (int)(e - s));
        if (type != 't') {
            ++codePtr->nscripts;
            len = -len;
        }
        Tcl_DStringAppend(&parsePtr->lens, (char *) &len, LENSZ);
        Tcl_DStringAppend(&parsePtr->lines, (char *) &parsePtr->line, LENSZ);
        while (s < e) {
            if (*s++ == '\n') {
                ++parsePtr->line;
            }
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetTag --
 *
 *      Copy tag name in lowercase to given dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Start of att=val pairs, if any, are set is aPtr if not null.
 *
 *----------------------------------------------------------------------
 */

static void
GetTag(Tcl_DString *dsPtr, char *s, const char *e, char **aPtr)
{
    char *t;

    assert(dsPtr != NULL);
    assert(s != NULL);
    assert(e != NULL);

    ++s;
    while (s < e && isspace(UCHAR(*s))) {
        ++s;
    }
    t = s;
    while (s < e  && !isspace(UCHAR(*s))) {
        ++s;
    }
    Tcl_DStringTrunc(dsPtr, 0);
    Tcl_DStringAppend(dsPtr, t, (int)(s - t));
    if (aPtr != NULL) {
	while (s < e && isspace(UCHAR(*s))) {
	    ++s;
	}
	*aPtr = s;
    }
    dsPtr->length = Tcl_UtfToLower(dsPtr->string);
}


/*
 *----------------------------------------------------------------------
 *
 * ParseAtts --
 *
 *      Parse tag attributes, either looking for known <script>
 *      pairs or copying cleaned up pairs to given dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Flags in given flagsPtr are updated and/or data copied to given
 *      dstring.
 *
 *----------------------------------------------------------------------
 */

static void
ParseAtts(char *s, const char *e, unsigned int *flagsPtr, Tcl_DString *attsPtr, int atts)
{
    char *vs = NULL, *as = NULL, *ve = NULL;
    char end = '\0', vsave = '\0';

    assert(s != NULL);
    assert(e != NULL);

    if (flagsPtr != NULL) {
        *flagsPtr = 0U;
    }
    while (s < e) {
	char asave, *ae;

        /*
         * Trim attribute name.
         */

        while (s < e && isspace(UCHAR(*s))) {
            ++s;
        }
        if (s == e) {
            break;
        }
        as = s;

        if (*s != '\'' && *s != '"') {
            while (s < e && !isspace(UCHAR(*s)) && *s != '=') {
                ++s;
            }
        } else {
            ++s;
            while (s < e && *s != *as) {
                ++s;
            }
            ++s;
        }

        ae = s;
        while (s < e && isspace(UCHAR(*s))) {
            ++s;
        }
        if (*s != '=') {
            /*
             * Use attribute name as value.
             */

            vs = as;
        } else {
            /*
             * Trim spaces and/or quotes from value.
             */

            do {
                ++s;
            } while (s < e && isspace(UCHAR(*s)));
            vs = s;

            if (*s != '"' && *s != '\'') {
                while (s < e && !isspace(UCHAR(*s))) {
                    ++s;
                }
            } else {
                ++s;
                while (s < e && *s != *vs) {
                    ++s;
                }
                ++s;
            }

            ve = s;
            end = *vs;
            if (end != '=' && end != '"' && end != '\'') {
                end = '\0';
            }
            if (end != '\0' && ve > vs && ve[-1] == end) {
                ++vs;
                --ve;
            }
            vsave = *ve;
            *ve = '\0';
        }
        asave = *ae;
        *ae = '\0';

        /*
         * Append attributes or scan for special <script> pairs.
         */

        if (attsPtr != NULL) {
            if (atts) {
                Tcl_DStringAppendElement(attsPtr, as);
            }
            Tcl_DStringAppendElement(attsPtr, vs);
        }
        if (flagsPtr != NULL && vs != as) {
            if (STRIEQ(as, "runat") && STRIEQ(vs, "server")) {
                *flagsPtr |= SERV_RUNAT;
            } else if (STRIEQ(as, "language") && !STRIEQ(vs, "tcl")) {
                *flagsPtr |= SERV_NOTTCL;
            } else if (STRIEQ(as, "stream") && STRIEQ(vs, "on")) {
                *flagsPtr |= SERV_STREAM;
            }
        }

        /*
         * Restore strings.
         */

        *ae = asave;
        if (vs != as) {
            *ve = vsave;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetScript --
 *
 *      Parse tag for a possible server-based <script>.
 *
 * Results:
 *      Pointer to start of script or NULL if not a <script> tag.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
GetScript(const char *tag, char *a, char *e, unsigned int *streamFlagPtr)
{
    unsigned int flags;

    assert(tag != NULL);
    assert(a != NULL);
    assert(e != NULL);
    assert(streamFlagPtr != NULL);

    if (a < e && STRIEQ(tag, "script")) {
        ParseAtts(a, e, &flags, NULL, 1);
        if ((flags & SERV_RUNAT) && !(flags & SERV_NOTTCL)) {
            *streamFlagPtr = (flags & SERV_STREAM);
            return (e + 1);
        }
    }
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * AppendTag --
 *
 *      Append tag script block.
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
AppendTag(Parse *parsePtr, const Tag *tagPtr, char *as, const char *ae, char *se, unsigned int flags)
{
    Tcl_DString script;

    assert(parsePtr != NULL);
    assert(tagPtr != NULL);
    assert(ae != NULL);

    Tcl_DStringInit(&script);
    Tcl_DStringAppend(&script, "ns_adp_append [", -1);
    if (tagPtr->type == TAG_ADP) {
        /* NB: String will be an ADP fragment to evaluate. */
        Tcl_DStringAppend(&script, "ns_adp_eval ", -1);
    }
    Tcl_DStringAppendElement(&script, tagPtr->string);
    if (tagPtr->type == TAG_PROC) {
        /* NB: String was a procedure, append tag attributes. */
        ParseAtts(as, ae, NULL, &script, 0);
    }
    if (se != NULL && se > ae) {
        /* NB: Append enclosing text as argument to eval or proc. */
        char save = *se;
        *se = '\0';
        Tcl_DStringAppendElement(&script, ae + 1);
        *se = save;
    }
    if (tagPtr->type == TAG_SCRIPT || tagPtr->type == TAG_ADP) {
        /* NB: Append code to create set with tag attributes. */
        Tcl_DStringAppend(&script, " [ns_set create", -1);
        Tcl_DStringAppendElement(&script, tagPtr->tag);
        ParseAtts(as, ae, NULL, &script, 1);
        Tcl_DStringAppend(&script, "]", 1);
    }
    /* NB: Close ns_adp_append subcommand. */
    Tcl_DStringAppend(&script, "]", 1);
    AppendBlock(parsePtr, script.string, script.string+script.length, 's', flags);
    Tcl_DStringFree(&script);
}


/*
 *----------------------------------------------------------------------
 *
 * AppendLengths --
 *
 *      Append the block length and line numbers to the given
 *      parse code.
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
AppendLengths(AdpCode *codePtr, const int *length, const int *line)
{
    Tcl_DString *textPtr;
    int          start, ncopy;

    assert(codePtr != NULL);
    assert(length != NULL);
    assert(line != NULL);

    textPtr = &codePtr->text;
    /* NB: Need to round up start of lengths array to next word. */
    start = ((textPtr->length / LENSZ) + 1) * LENSZ;
    ncopy = codePtr->nblocks * LENSZ;
    Tcl_DStringSetLength(textPtr, start + (ncopy * 2));
    codePtr->len = (int *) (textPtr->string + start);
    codePtr->line = (int *) (textPtr->string + start + ncopy);
    memcpy(codePtr->len,  length, (size_t) ncopy);
    memcpy(codePtr->line, line, (size_t) ncopy);
}
