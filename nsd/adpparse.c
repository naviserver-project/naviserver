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
 * adpparse.c --
 *
 *      ADP parser.
 */

#include "nsd.h"

#define SCRIPT_TAG_FOUND       0x01u
#define SCRIPT_TAG_SERV_STREAM 0x02u
#define SCRIPT_TAG_SERV_RUNAT  0x04u
#define SCRIPT_TAG_SERV_NOTTCL 0x08u

#define TAG_ADP     1
#define TAG_PROC    2
#define TAG_SCRIPT  3

#define APPEND      "ns_adp_append "
#define APPEND_LEN  (sizeof(APPEND) - 1u)

#define LENGTH_ELEM_SIZE  ((int)sizeof(TCL_SIZE_T))
#define LINE_ELEM_SIZE    ((int)sizeof(int))

typedef enum {
    TagInlineCode,
    TagNext,
    TagScript,
    TagReg
} TagParseState;

/*
 * The following structure maintains proc and ADP registered tags.
 * String bytes directly follow the Tag struct in the same allocated
 * block.
 */

typedef struct Tag {
    int            type;    /* Type of tag, ADP or proc. */
    char          *tag;     /* The name of the tag (e.g., "mytag") */
    char          *endtag;  /* The closing tag or null (e.g., "/mytag")*/
    char          *content; /* Proc (e.g., "ns_adp_netscape") or ADP string. */
} Tag;

/*
 * The following structure maintains state while parsing an ADP block.
 */

typedef struct Parse {
    AdpCode       *codePtr; /* Pointer to compiled AdpCode struct. */
    int            line;    /* Current line number while parsing. */
    Tcl_DString    lengths; /* Length of text or script block. */
    Tcl_DString    lines;   /* Line number of block for debug messages. */
} Parse;

/*
 * Local functions defined in this file
 */

static void AppendBlock(Parse *parsePtr, const char *s, char *e, char type, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void AppendTag(Parse *parsePtr, const Tag *tagPtr, char *as, const char *ae, char *se, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)  NS_GNUC_NONNULL(4);

static int RegisterObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, int type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AppendLengths(AdpCode *codePtr,
                          const void *length_bytes, size_t length_nbytes,
                          const void *line_bytes,   size_t line_nbytes)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static void GetTag(Tcl_DString *dsPtr, char *s, const char *e, char **aPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static char *GetScript(const char *tag, char *a, char *e, unsigned int *flagPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static void ParseAtts(char *s, const char *e, unsigned int *flagsPtr, Tcl_DString *attsPtr, int atts)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AdpParseAdp(AdpCode *codePtr, NsServer *servPtr, char *adp, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void AdpParseTclFile(AdpCode *codePtr, const char *adp, unsigned int flags, const char* file)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * report --
 *
 *      Local debugging function
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Printing to log file.
 */
#if 0
static void report(const char *msg, const char *string, ssize_t len)
{
    const int max = 3000;

    if (len == TCL_INDEX_NONE) {
        len = (ssize_t)strlen(string)+1;
    }
    {
        char buffer[len + 2];
        memcpy(buffer, string, len+1);
        buffer[len>max ? max : len+1] = '\0';
        Ns_Log(Notice, "%s //%s//", msg, buffer);
    }
}
#endif

/*
 * TagValidFirstChar, TagValidChar --
 *
 *      Valid characters for tag names. These rules are slightly more tolerant
 *      than in HTML, but these this is necessary, since ADP is more tolerant
 *      than HTML and supports as well embedding of tags in start tags,
 *      etc. This is as well needed for backward compatibility. These rules
 *      are in essence just needed in NsParseTagEnd to determine, if markup is
 *      used in attribute values.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      Printing to log file.
 */
static bool TagValidFirstChar (char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}
static bool TagValidChar (char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || (c == ':')
        || (c == '_') ;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAdpRegisterAdpObjCmd, NsTclAdpRegisterTagObjCmd, NsTclAdpRegisterProcObjCmd,
 * NsTclAdpRegisterScriptObjCmd, NsTclAdpRegisterAdptagObjCmd --
 *
 *      Implements "ns_adp_registeradp", "ns_adp_registertag",
 *      "ns_adp_registerproc", "ns_adp_registerscript", and
 *      "ns_register_adptag".
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
NsTclAdpRegisterAdpObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return RegisterObjCmd(clientData, interp, objc, objv, TAG_ADP);
}

#ifdef NS_WITH_DEPRECATED
int
NsTclAdpRegisterTagObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_LogDeprecated(objv, 1, "ns_adp_registeradp", NULL);
    return RegisterObjCmd(clientData, interp, objc, objv, TAG_ADP);
}
#endif

int
NsTclAdpRegisterProcObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return RegisterObjCmd(clientData, interp, objc, objv, TAG_PROC);
}

int
NsTclAdpRegisterScriptObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return RegisterObjCmd(clientData, interp, objc, objv, TAG_SCRIPT);
}

#ifdef NS_WITH_DEPRECATED
int
NsTclAdpRegisterAdptagObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_LogDeprecated(objv, 1, "ns_adp_registerscript", NULL);
    return RegisterObjCmd(clientData, interp, objc, objv, TAG_SCRIPT);
}
#endif

/*
 * The actual function doing the hard work.
 */
static int
RegisterObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, int type)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(clientData != NULL);
    NS_NONNULL_ASSERT(interp != NULL);


    if (objc != 4 && objc != 3) {
        if (type != TAG_ADP) {
            Tcl_WrongNumArgs(interp, 1, objv, "/tag/ ?/endtag/? /proc/");
        } else {
            Tcl_WrongNumArgs(interp, 1, objv, "/tag/ ?/endtag/? /adpstring/");
        }
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        const char     *end, *tag, *content;
        Tcl_HashEntry  *hPtr;
        int             isNew;
        TCL_SIZE_T      slen, elen, tlen;
        Tcl_DString     tbuf;
        Tag            *tagPtr;

        /*
         * Get the content
         */
        content = Tcl_GetStringFromObj(objv[objc-1], &slen);
        ++slen;
        if (objc == 3) {
            /*
             * no end tag
             */
            end = NULL;
            elen = 0;
        } else {
            /*
             * end tag provided.
             */
            end = Tcl_GetStringFromObj(objv[2], &elen);
            ++elen;
        }
        /*fprintf(stderr, "=========== RegisterObjCmd tag '%s', content '%s', end '%s'\n",
                Tcl_GetString(objv[1]),
                content, end);*/

        /*
         * Allocate piggybacked memory chunk containing
         *   - tag structure,
         *   - tag begin string,
         *   - tag end string
         */
        tagPtr = ns_malloc(sizeof(Tag) + (size_t)slen + (size_t)elen);
        if (unlikely(tagPtr == NULL)) {
            return TCL_ERROR;
        }

        tagPtr->type = type;
        tagPtr->content = (char *)tagPtr + sizeof(Tag);
        memcpy(tagPtr->content, content, (size_t) slen);
        (void)Tcl_UtfToLower(tagPtr->content);
        if (end == NULL) {
            tagPtr->endtag = NULL;
        } else {
            tagPtr->endtag = tagPtr->content + slen;
            memcpy(tagPtr->endtag, end, (size_t) elen);
            (void)Tcl_UtfToLower(tagPtr->endtag);
        }

        /*
         * Get the tag string and add it to the adp.tag table.
         */
        Tcl_DStringInit(&tbuf);
        tag = Tcl_GetStringFromObj(objv[1], &tlen);
        (void)Tcl_UtfToLower(Tcl_DStringAppend(&tbuf, tag, tlen));
        Ns_RWLockWrLock(&servPtr->adp.taglock);
        hPtr = Tcl_CreateHashEntry(&servPtr->adp.tags, tbuf.string, &isNew);
        if (isNew == 0) {
            ns_free(Tcl_GetHashValue(hPtr));
        }
        Tcl_SetHashValue(hPtr, tagPtr);
        tagPtr->tag = Tcl_GetHashKey(&servPtr->adp.tags, hPtr);
        Ns_RWLockUnlock(&servPtr->adp.taglock);
        Tcl_DStringFree(&tbuf);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpParse --
 *
 *      Parse a string containing Tcl statements. When evaluating a Tcl file,
 *      we just wrap it as Tcl proc and save in ADP block with cache enabled
 *      or just execute the Tcl code in case of cache disabled
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The given AdpCode structure is filled in with a copy of the Tcl source
 *      code.
 *
 *----------------------------------------------------------------------
 */

static void
AdpParseTclFile(AdpCode *codePtr, const char *adp, unsigned int flags, const char* file) {
    int        line = 0;
    TCL_SIZE_T size;

    NS_NONNULL_ASSERT(codePtr != NULL);
    NS_NONNULL_ASSERT(adp != NULL);

    if ((flags & ADP_CACHE) == 0u) {
        Tcl_DStringAppend(&codePtr->text, adp, TCL_INDEX_NONE);
    } else {
        Ns_DStringPrintf(&codePtr->text,
                         "ns_adp_append {<%%"
                         "if {[info proc adp:%s] == {}} {"
                         "  proc adp:%s {} { uplevel [for {", file, file);
        Tcl_DStringAppend(&codePtr->text, adp, TCL_INDEX_NONE);
        Ns_DStringPrintf(&codePtr->text, "} {0} {} {}]}}\nadp:%s %%>}", file);
    }
    codePtr->nblocks = codePtr->nscripts = 1;
    size = -codePtr->text.length;
    AppendLengths(codePtr, &size, sizeof(size), &line, sizeof(line));
}

/*
 *----------------------------------------------------------------------
 *
 * NsParseTagEnd --
 *
 *      Search for the endsign of a tag (greater than). For ages, NaviServer
 *      just searched for the first upcoming greater than sign:
 *
 *         strchr(str, INTCHAR('>'));
 *
 *      However, the Living Standard of HTML allows the greater sign in
 *      attributes values as long these are between single or double quotes:
 *      https://html.spec.whatwg.org/multipage/syntax.html#syntax-attribute-value
 *
 *      As long the tag looks like a valid definition, it parses it and ignores
 *      markup between quotes. When the passed-in string does not look like a
 *      well-formed start tag, fall back to the legacy approach to provide
 *      maximal backward compatibility.
 *
 * Results:
 *      Either pointing the ending greater sign or NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
char*
NsParseTagEnd(char *str)
{
    const char *startTagStr = str;
    /*
     * Parse tag name
     */
    str++;
    if (!TagValidFirstChar(*str)) {
        //Ns_Log(Notice, "legacy case: no valid first character of tag name");
        goto legacy;
    }
    str++;
    while (TagValidChar(*str)) {
        str++;
    }
    //report("NsParseTagEnd tag name ", startTagStr, str-startTagStr);

    /*
     * Now we expect whitespace* followed by optional attributes and maybe the
     * closing ">" character.
     */
    if (*str != '>' && CHARTYPE(space, *str) == 0) {
        //Ns_Log(Notice, "legacy case: no space or > after tag name");
        goto legacy;
    }
    for (;;) {
        while (CHARTYPE(space, *str) != 0) {
            str++;
        }
        if (*str == '>') {
            goto done;
        }
        /*
         * Expect attribute name
         */
        if (!TagValidFirstChar(*str)) {
            //Ns_Log(Notice, "legacy case: no valid first character of attribute name");
            goto legacy;
        }
        str++;
        while (TagValidChar(*str)) {
            str++;
        }
        while (CHARTYPE(space, *str) != 0) {
            str++;
        }
        //report("NsParseTagEnd tag name + att", startTagStr, str-startTagStr);
        if (*str == '>') {
            /*
             * Attribute without equal sign at the end
             */
            goto done;
        }
        if (*str != '=') {
            //Ns_Log(Notice, "legacy case: no valid character after attribute name");
            goto legacy;
        }
        str++;
        while (CHARTYPE(space, *str) != 0) {
            str++;
        }
        /*
         * We expect quoted or unquoted attribute value
         */
        //Ns_Log(Notice, "We are now at char '%c'. Is this a quoted value?", *str);
        if (*str == '\'' || *str == '"') {
            const char quote = *str;
            str++;
            while (*str != quote) {
                if (*str == '\0') {
                    //Ns_Log(Notice, "legacy case: quote not terminated");
                    goto legacy;
                }
                str++;
            }
            str++;
        } else {
            /*
             * Unquoted value
             */
            if (!TagValidFirstChar(*str)) {
                //Ns_Log(Notice, "legacy case: no valid first character of unquoted value");
                goto legacy;
            }
            str++;
            while (TagValidChar(*str)) {
                str++;
            }
        }
        //report("NsParseTagEnd tag name + att + value", startTagStr, str-startTagStr);
    }
    assert(str != NULL);
    //report("NsParseTagEnd ===", startTagStr, str-startTagStr);

 done:
    return str;

 legacy:

    str = strchr(startTagStr, INTCHAR('>'));
    /*
    if (str != NULL) {
        report("NsParseTagEnd ===", startTagStr, str-startTagStr);
    } else {
        Ns_Log(Notice, "NsParseTagEnd === NULL");
        }*/

    return str;
}


/*
 *----------------------------------------------------------------------
 *
 * AdpParseAdp --
 *
 *      Parse a string of ADP text/script.  Parsing is done in a single,
 *      top to bottom pass, looking for the following four types of
 *      embedded script sequences:
 *
 *      1. <% Tcl script %>
 *      2. <script runat=server language=tcl> Tcl script </script>
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
 *      The given AdpCode structure is filled in with copy
 *      of the parsed ADP code.
 *
 *----------------------------------------------------------------------
 */

static void
AdpParseAdp(AdpCode *codePtr, NsServer *servPtr, char *adp, unsigned int flags)
{
    int                  level = 0;
    unsigned int         scriptFlags = 0u;
    const char          *script = NS_EMPTY_STRING, *ae = NS_EMPTY_STRING;
    char                *s, *e, *a, *text, null = '\0', *as = &null;
    const Tag           *tagPtr = NULL;
    Tcl_DString          tag;
    bool                 scriptStreamDone = NS_FALSE;
    Parse                parse;
    TagParseState        state = TagNext;

    NS_NONNULL_ASSERT(codePtr != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(adp != NULL);

    /*
     * Initialize the parse structure.
     */
    parse.codePtr = codePtr;
    parse.line = 0;

    Tcl_DStringInit(&tag);
    Tcl_DStringInit(&parse.lengths);
    Tcl_DStringInit(&parse.lines);

    /*
     * Parse ADP one tag at a time.
     */
    text = adp;
    Ns_RWLockRdLock(&servPtr->adp.taglock);

    for (;;) {

        s = strchr(adp, INTCHAR('<'));
        if (s == NULL) {
            break;
        }

        /*
         * Process the tag depending on the current state.
         */
        switch (state) {

        case TagInlineCode:
            /*
             * We identified the start of a <% ... %> block. Find the
             * corresponding %> beyond any additional nested <% ... %>
             * sequences.
             *
             * Handling of <% ...%> requires a different end-of-tag
             * handling. For regular tags, we have to differentiate
             * between the closing ">" inside and outside quotes, which
             * does not apply the adp-eval blocks.
             */

            e = strstr(s, "%>");
            if (e != NULL) {
                const char *n;

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
                    if ((flags & ADP_SAFE) == 0u) {
                        if (s[2] != '=') {
                            AppendBlock(&parse, s + 2, e, 's', flags);
                        } else {
                            AppendBlock(&parse, s + 3, e, 'S', flags);
                        }
                    }
                    text = e + 2; /* NB: Next text after valid closing %>. */
                }

                state = TagNext;

                /*
                 * Skip to next possible ADP tag location.
                 */
                adp = text;
                continue;
            }
            break;

        case TagNext:
            /*
             * Do we have a regular tag or a <% ... %> block?
             */
            if (s[1] == '%' && s[2] != '>') /* NB: Avoid <%>. */ {
                /*
                 * Just switch to the inline code block state and continue.
                 */
                state = TagInlineCode;
                continue;
            }
            if (!(
                  (s[1] >= 'a' && s[1] <= 'z')
                  || (s[1] >= 'A' && s[1] <= 'Z')
                  || (s[1] >= '0' && s[1] <= '9')
                  )) {
                //report("state TagNext, invalid begin of tag", s, TCL_INDEX_NONE);
                adp = s + 1;
                continue;
            }

            /*
             * Is this a start tag <START_TAG A1="..." ...>?
             */
            //e = strchr(s, INTCHAR('>'));
            e = NsParseTagEnd(s);
            if (e != NULL) {
                /*
                 * Check for <script> tags or registered tags.
                 */
                //report("state TagNext, parse tag", s, e-s);

                GetTag(&tag, s, e, &a);
                script = GetScript(tag.string, a, e, &scriptFlags);
                if (script != NULL) {
                    /*
                     * Append text and begin looking for closing </script> tag.
                     */
                    AppendBlock(&parse, text, s, 't', flags);
                    state = TagScript;
                    level = 1;
                } else {
                    const Tcl_HashEntry *hPtr;

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
                /*
                 * Skip to next possible ADP tag location.
                 */
                //Ns_Log(Notice, "found tag '%s' len %d", tag.string, tag.length);
                //adp = s + tag.length + 1;
                //report("YY advance to ", adp, e-adp);
                //adp = s + 1;
                //report("... instead of ", adp, e-adp);
                adp = s + tag.length + 1;
                continue;
            }
            break;

        case TagScript:
            /*
             * We are inside a script tag.
             */
            e = strchr(s, INTCHAR('>'));
            if (e != NULL) {
                //report("state TagScript, parse tag", s, e-s);

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

                        if ((flags & ADP_SAFE) == 0u) {
                            if (((scriptFlags & SCRIPT_TAG_SERV_STREAM) != 0u) && (! scriptStreamDone)) {
                                static char buffer[] = "ns_adp_ctl stream on";
                                char *end = buffer + strlen(buffer);

                                AppendBlock(&parse, buffer, end, 's', flags);
                                scriptStreamDone = NS_TRUE;
                            }
                            AppendBlock(&parse, script, s, 's', flags);
                        }
                        text = e + 1;
                        state = TagNext;
                    }
                }
                /*
                 * Skip to next possible ADP tag location.
                 */
                //Ns_Log(Notice, "found tag '%s' len %d", tag.string, tag.length);
                adp = s + tag.length + 1;
                continue;
            }
            break;

        case TagReg:
            /*
             * We are inside a registered tag
             */
            e = strchr(s, INTCHAR('>'));
            if (e != NULL) {
                //report("state TagReg, parse tag", s, e-s);

                /*
                 * Looking for corresponding closing tag for a registered
                 * tag, handling possible nesting of the same tag.
                 */
                GetTag(&tag, s, e, NULL);
                if (STREQ(tag.string, tagPtr->tag)) {
                    /*
                     * Nesting of the same tag.
                     */
                    ++level;
                    adp = s + tag.length + 1;

                } else if (STREQ(tag.string, tagPtr->endtag)) {
                    /*
                     * Closing tag.
                     */
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
                    adp = s + tag.length + 2;
                } else {
                    adp = s + 1;
                }
                continue;
            }
            break;
        }

        break;
    }
    Ns_RWLockUnlock(&servPtr->adp.taglock);

    /*
     * Append the remaining text block
     */
    assert(text != NULL);
    {
        size_t len = strlen(text);
        if (len > 0u) {
            AppendBlock(&parse, text, text + len, 't', flags);
        }
    }
    /*
     * If requested, collapse blocks to a single Tcl script and
     * and complete the parse code structure.
     */

    if ((flags & ADP_SINGLE) != 0u) {
        /*
         * See also: AdpParseTclFile(), and AdpExec() in adpeval.c
         */
        int        line = 0;
        TCL_SIZE_T len = -codePtr->text.length;

        codePtr->nscripts = codePtr->nblocks = 1;
        AppendLengths(codePtr, &len, sizeof(len), &line, sizeof(line));
    } else {
        AppendLengths(codePtr,
                      Tcl_DStringValue(&parse.lengths),
                      (size_t)Tcl_DStringLength(&parse.lengths),
                      Tcl_DStringValue(&parse.lines),
                      (size_t)Tcl_DStringLength(&parse.lines));
    }

    Tcl_DStringFree(&parse.lengths);
    Tcl_DStringFree(&parse.lines);
    Tcl_DStringFree(&tag);
}


/*
 *----------------------------------------------------------------------
 *
 * AdpParseAdp --
 *
 *      Parse a string containing a Tcl source or an ADP text/script.
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
           unsigned int flags, const char* file)
{
    NS_NONNULL_ASSERT(codePtr != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(adp != NULL);

    /*
     * Initialize the code structure.
     */
    Tcl_DStringInit(&codePtr->text);
    codePtr->nscripts = codePtr->nblocks = 0;

    /*
     * Special case when we evaluating Tcl file, we just wrap it as
     * Tcl proc and save in ADP block with cache enabled or
     * just execute the Tcl code in case of cache disabled
     */
    if ((flags & ADP_TCLFILE) != 0u) {
        AdpParseTclFile(codePtr, adp, flags, file);
    } else {
        AdpParseAdp(codePtr, servPtr, adp, flags);
    }
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
    NS_NONNULL_ASSERT(codePtr != NULL);

    Tcl_DStringFree(&codePtr->text);
    codePtr->nblocks = codePtr->nscripts = 0;
    codePtr->len = NULL;
    codePtr->line = NULL;
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

    NS_NONNULL_ASSERT(parsePtr != NULL);
    NS_NONNULL_ASSERT(s != NULL);
    NS_NONNULL_ASSERT(e != NULL);
    NS_NONNULL_ASSERT(s <= e);

    len = e - s;

    if (likely(len > 0)) {

        codePtr = parsePtr->codePtr;

        if ((flags & ADP_SINGLE) != 0u) {
            char save;

            switch (type) {
            case 'S':
                Tcl_DStringAppend(&codePtr->text, APPEND, (TCL_SIZE_T)APPEND_LEN);
                Tcl_DStringAppend(&codePtr->text, s, (TCL_SIZE_T)len);
                break;

            case 't':
                save = *e;
                *e = '\0';
                Tcl_DStringAppend(&codePtr->text, APPEND, (TCL_SIZE_T)APPEND_LEN);
                Tcl_DStringAppendElement(&codePtr->text, s);
                *e = save;
                break;

            default:
                Tcl_DStringAppend(&codePtr->text, s, (TCL_SIZE_T)len);

            }
            Tcl_DStringAppend(&codePtr->text, "\n", 1);

        } else {
            ptrdiff_t  l = len;

            ++codePtr->nblocks;
            if (type == 'S') {
                l += (ptrdiff_t)APPEND_LEN;
                Tcl_DStringAppend(&codePtr->text, APPEND, (TCL_SIZE_T)APPEND_LEN);
            }
            Tcl_DStringAppend(&codePtr->text, s, (TCL_SIZE_T)len);
            if (type != 't') {
                ++codePtr->nscripts;
                l = -l;
            }
            Tcl_DStringAppend(&parsePtr->lengths, (const char *)&l,              LENGTH_ELEM_SIZE);
            Tcl_DStringAppend(&parsePtr->lines,   (const char *)&parsePtr->line, LINE_ELEM_SIZE);
            /*
             * Increment line numbers based on the passed-in segment
             */
            while( ((s = strchr(s, INTCHAR('\n'))) != NULL) && (s < e)) {
                ++parsePtr->line;
                ++s;
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
    const char *t;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(s != NULL);
    NS_NONNULL_ASSERT(e != NULL);

    ++s;
    while (s < e && CHARTYPE(space, *s) != 0) {
        ++s;
    }
    t = s;
    /*
     * The following loop for obtaining the tag name is more liberal than the
     * HTML specification, allowing just letters and digits. However, we do
     * NOT want e.g. "html<if" as a tag name, whenparsing "<html<if ...>>"
     */
    while (s < e  && CHARTYPE(space, *s) == 0  && *s != '<') {
        ++s;
    }
    Tcl_DStringSetLength(dsPtr, 0);
    Tcl_DStringAppend(dsPtr, t, (TCL_SIZE_T)(s - t));
    if (aPtr != NULL) {
        while (s < e && CHARTYPE(space, *s) != 0) {
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
    char  *ve = NULL, vsave = '\0';

    NS_NONNULL_ASSERT(s != NULL);
    NS_NONNULL_ASSERT(e != NULL);

    if (flagsPtr != NULL) {
        *flagsPtr = 0u;
    }
    while (s < e) {
        char asave, *ae, *as, *vs;

        /*
         * Trim attribute name.
         */
        while (s < e && CHARTYPE(space, *s) != 0) {
            ++s;
        }
        if (s == e) {
            break;
        }
        as = s;

        if (*s != '\'' && *s != '"') {
            while (s < e && CHARTYPE(space, *s) == 0 && *s != '=') {
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
        while (s < e && CHARTYPE(space, *s) != 0) {
            ++s;
        }
        if (*s != '=') {
            /*
             * Use attribute name as value.
             */

            vs = as;
        } else {
            char end;

            /*
             * Trim spaces and/or quotes from value.
             */

            do {
                ++s;
            } while (s < e && CHARTYPE(space, *s) != 0);
            vs = s;

            if (*s != '"' && *s != '\'') {
                while (s < e && CHARTYPE(space, *s) == 0) {
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
            if (atts != 0) {
                Tcl_DStringAppendElement(attsPtr, as);
            }
            Tcl_DStringAppendElement(attsPtr, vs);
        }
        if (flagsPtr != NULL && vs != as) {
            if (STRIEQ(as, "runat") && STRIEQ(vs, "server")) {
                *flagsPtr |= SCRIPT_TAG_SERV_RUNAT;
            } else if (STRIEQ(as, "language") && !STRIEQ(vs, "tcl")) {
                *flagsPtr |= SCRIPT_TAG_SERV_NOTTCL;
            } else if (STRIEQ(as, "stream") && STRIEQ(vs, "on")) {
                *flagsPtr |= SCRIPT_TAG_SERV_STREAM;
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
GetScript(const char *tag, char *a, char *e, unsigned int *flagPtr)
{
    unsigned int flags;
    char        *result = NULL;

    NS_NONNULL_ASSERT(tag != NULL);
    NS_NONNULL_ASSERT(a != NULL);
    NS_NONNULL_ASSERT(e != NULL);
    NS_NONNULL_ASSERT(flagPtr != NULL);

    if (a < e && STRIEQ(tag, "script")) {
        ParseAtts(a, e, &flags, NULL, 1);
        if ((flags & SCRIPT_TAG_SERV_RUNAT) != 0u && (flags & SCRIPT_TAG_SERV_NOTTCL) == 0u) {
            *flagPtr = (flags & SCRIPT_TAG_SERV_STREAM);
            result = (e + 1);
        }
    }
    return result;
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

    NS_NONNULL_ASSERT(parsePtr != NULL);
    NS_NONNULL_ASSERT(tagPtr != NULL);
    NS_NONNULL_ASSERT(as != NULL);
    NS_NONNULL_ASSERT(ae != NULL);

    Tcl_DStringInit(&script);
    Tcl_DStringAppend(&script, "ns_adp_append [", TCL_INDEX_NONE);
    if (tagPtr->type == TAG_ADP) {
        /*
         * String will be an ADP fragment to evaluate.
         */
        Tcl_DStringAppend(&script, "ns_adp_parse -- ", TCL_INDEX_NONE);
    }
    Tcl_DStringAppendElement(&script, tagPtr->content);
    if (tagPtr->type == TAG_PROC) {
        /*
         * String was a procedure, append tag attributes.
         */
        ParseAtts(as, ae, NULL, &script, 0);
    }
    if (se != NULL && se > ae) {
        /*
         * Append enclosing text as argument to eval or proc.
         */
        char save = *se;

        *se = '\0';
        Tcl_DStringAppendElement(&script, ae + 1);
        *se = save;
    }
    if (tagPtr->type == TAG_SCRIPT || tagPtr->type == TAG_ADP) {
        /*
         * Append code to create set with tag attributes.
         */
        Tcl_DStringAppend(&script, " [ns_set create", TCL_INDEX_NONE);
        Tcl_DStringAppendElement(&script, tagPtr->tag);
        ParseAtts(as, ae, NULL, &script, 1);
        Tcl_DStringAppend(&script, "]", 1);
    }
    /*
     * Close ns_adp_append subcommand.
     */
    Tcl_DStringAppend(&script, "]", 1);
    AppendBlock(parsePtr, script.string, script.string+script.length, 's', flags);
    Tcl_DStringFree(&script);
}

/*
 * AppendLengths --
 *
 *    Append block length and line number arrays to the backing
 *    Tcl_DString of an AdpCode structure, storing them in an aligned
 *    fashion. This ensures that typed pointers to length and line arrays
 *    are safely embedded within the same contiguous buffer as the code
 *    text.
 *
 * Arguments:
 *    codePtr       - Pointer to the AdpCode structure whose text buffer
 *                    will be extended.
 *    length_bytes  - Raw bytes for block lengths (array of TCL_SIZE_T).
 *    length_nbytes - Number of bytes provided in length_bytes.
 *    line_bytes    - Raw bytes for line numbers (array of int).
 *    line_nbytes   - Number of bytes provided in line_bytes.
 *
 * Returns:
 *    None.
 *
 * Side Effects:
 *    - Resizes codePtr->text to make room for aligned arrays.
 *    - Updates codePtr->len and codePtr->line to point into the
 *      newly appended storage.
 *    - Copies caller-provided arrays into this space, truncating if fewer
 *      bytes are provided than required for codePtr->nblocks.
 *
 * Notes:
 *    - Alignment is computed as the stricter of alignof(TCL_SIZE_T) and
 *      alignof(int).
 *    - The arrays are valid only as long as the DString is not further
 *      resized or freed.
 *    - Caller must ensure that codePtr->nblocks is set consistently with
 *      the contents of length_bytes and line_bytes.
 */
static void
AppendLengths(AdpCode *codePtr,
              const void *length_bytes, size_t length_nbytes,
              const void *line_bytes,   size_t line_nbytes)
{
    char        *base, *start, *mid;
    Tcl_DString *textPtr   = &codePtr->text;
    TCL_SIZE_T   oldLen    = textPtr->length;
    size_t       need_len  = (size_t)codePtr->nblocks * sizeof(TCL_SIZE_T);
    size_t       need_line = (size_t)codePtr->nblocks * sizeof(int);
    const size_t a         = NS_ALIGNOF(TCL_SIZE_T) > NS_ALIGNOF(int)
        ? NS_ALIGNOF(TCL_SIZE_T)
        : NS_ALIGNOF(int);

    NS_NONNULL_ASSERT(codePtr != NULL);
    NS_NONNULL_ASSERT(length_bytes != NULL);
    NS_NONNULL_ASSERT(line_bytes != NULL);

    /* Truncate if caller provided less than needed */
    if (length_nbytes < need_len)  {
        need_len  = length_nbytes;
    }
    if (line_nbytes < need_line) {
        need_line = line_nbytes;
    }

    /* Reserve padding for alignment + storage for both byte arrays */
    Tcl_DStringSetLength(textPtr, oldLen + (TCL_SIZE_T)(a - 1 + need_len + need_line));

    /* Compute aligned start within (possibly reallocated) buffer */
    base   = textPtr->string + oldLen;
    start  = (char *)ns_align_up(base, a);
    mid    = start + need_len;   /* second array immediately after the first */

    /* Set the typed pointers without a cast that triggers -Wcast-align */
    memcpy(&codePtr->len,  &start, sizeof(codePtr->len));
    memcpy(&codePtr->line, &mid,   sizeof(codePtr->line));

    /* Copy the raw bytes in */
    memcpy(codePtr->len,  length_bytes, need_len);
    memcpy(codePtr->line, line_bytes,   need_line);
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
