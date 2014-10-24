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
 * config.c --
 *
 *      Support for the configuration file
 */

#include "nsd.h"

#define ISSLASH(c) ((c) == '/' || (c) == '\\')

/*
 * Local functions defined in this file.
 */

static Tcl_CmdProc SectionCmd;
static Tcl_CmdProc ParamCmd;
static Ns_Set     *GetSection(CONST char *section, int create);
static char       *ConfigGet(CONST char *section, CONST char *key, int exact, CONST char *defstr);
static int         ToBool(CONST char *value, int *valuePtr);



/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigString --
 *
 *      Return a config file value, or the default if not found.
 *
 * Results:
 *      Pointer to value string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Ns_ConfigString(CONST char *section, CONST char *key, CONST char *def)
{
    CONST char *value;

    value = ConfigGet(section, key, 0, def);
    Ns_Log(Dev, "config: %s:%s value=\"%s\" default=\"%s\" (string)",
           section ? section : "", key, value ? value : "", 
           def ? def : "");

    return value ? value : def;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigBool --
 *
 *      Return a boolean config file value, or the default if not
 *      found.
 *
 * Results:
 *      NS_TRUE or NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConfigBool(CONST char *section, CONST char *key, int def)
{
    CONST char *s;
    int value = NS_FALSE, found = NS_FALSE;

    s = ConfigGet(section, key, 0, def ? "true" : "false");
    if (s != NULL && ToBool(s, &value)) {
        found = NS_TRUE;
    }
    Ns_Log(Dev, "config: %s:%s value=%s default=%s (bool)",
           section ? section : "", key,
           found ? (value ? "true" : "false") : "(null)",
           def ? "true" : "false");

    return found ? value : def;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigFlag --
 *
 *      Look for a boolean config file value, and if present OR the
 *      given flag value into the flagsPtr.
 *
 * Results:
 *      NS_TRUE if flag was set, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConfigFlag(CONST char *section, CONST char *key, unsigned int flag, int def,
              unsigned int *flagsPtr)
{
    CONST char *s;
    int value = 0, found = NS_FALSE;

    s = ConfigGet(section, key, 0, def ? "true" : "false");
    if (s != NULL && ToBool(s, &value)) {
        found = NS_TRUE;
    }

    Ns_Log(Dev, "config: %s:%s value=%d default=%d (flag)",
           section ? section : "", key,
           value ? flag : 0,
           def ? flag : 0);

    if (value) {
        *flagsPtr |= flag;
    }
    return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigInt, Ns_ConfigIntRange --
 *
 *      Return an integer config file value, or the default if not
 *      found. The returned value will be between the given min and max.
 *
 * Results:
 *      An integer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConfigInt(CONST char *section, CONST char *key, int def)
{
    return Ns_ConfigIntRange(section, key, def, INT_MIN, INT_MAX);
}

int
Ns_ConfigIntRange(CONST char *section, CONST char *key, int def,
                  int min, int max)
{
    CONST char *s;
    char defstr[TCL_INTEGER_SPACE];
    int value;

    snprintf(defstr, sizeof(defstr), "%d", def);
    s = ConfigGet(section, key, 0, defstr);
    if (s != NULL && Ns_StrToInt(s, &value) == NS_OK) {
        Ns_Log(Dev, "config: %s:%s value=%d min=%d max=%d default=%d (int)",
               section ? section : "", key, value, min, max, def);
    } else {
        Ns_Log(Dev, "config: %s:%s value=(null) min=%d max=%d default=%d (int)",
               section ? section : "", key, min, max, def);
        value = def;
    }
    if (value < min) {
        Ns_Log(Warning, "config: %s:%s value=%d, rounded up to %d",
               section ? section : "", key, value, min);
        value = min;
    }
    if (value > max) {
        Ns_Log(Warning, "config: %s:%s value=%d, rounded down to %d",
               section ? section : "", key, value, max);
        value = max;
    }
    return value;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_Configwide, Ns_ConfigWideRange --
 *
 *      Return an wide integer config file value, or the default if not
 *      found. The returned value will be between the given min and max.
 *
 * Results:
 *      An Tcl_WideInt integer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Tcl_WideInt
Ns_ConfigWideInt(CONST char *section, CONST char *key, Tcl_WideInt def)
{
    return Ns_ConfigWideIntRange(section, key, def, INT_MIN, INT_MAX);
}

Tcl_WideInt
Ns_ConfigWideIntRange(CONST char *section, CONST char *key, Tcl_WideInt def,
                  Tcl_WideInt min, Tcl_WideInt max)
{
    CONST char *s;
    char defstr[TCL_INTEGER_SPACE];
    Tcl_WideInt value;

    snprintf(defstr, sizeof(defstr), "%" TCL_LL_MODIFIER "d", def);
    s = ConfigGet(section, key, 0, defstr);
    if (s != NULL && Ns_StrToWideInt(s, &value) == NS_OK) {
        Ns_Log(Dev, "config: %s:%s value=%" TCL_LL_MODIFIER "d min=%" TCL_LL_MODIFIER "d max=%" TCL_LL_MODIFIER "d default=%" TCL_LL_MODIFIER "d (wide int)",
               section ? section : "", key, value, min, max, def);
    } else {
        Ns_Log(Dev, "config: %s:%s value=(null) min=%" TCL_LL_MODIFIER "d max=%" TCL_LL_MODIFIER "d default=%" TCL_LL_MODIFIER "d (wide int)",
               section ? section : "", key, min, max, def);
        value = def;
    }
    if (value < min) {
        Ns_Log(Warning, "config: %s:%s value=%" TCL_LL_MODIFIER "d, rounded up to %" TCL_LL_MODIFIER "d",
               section ? section : "", key, value, min);
        value = min;
    }
    if (value > max) {
        Ns_Log(Warning, "config: %s:%s value=%" TCL_LL_MODIFIER "d, rounded down to %" TCL_LL_MODIFIER "d",
               section ? section : "", key, value, max);
        value = max;
    }
    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetValue --
 *
 *      Return a config file value for a given key
 *
 * Results:
 *      ASCIIZ ptr to a value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConfigGetValue(CONST char *section, CONST char *key)
{
    char *value;

    value = ConfigGet(section, key, 0, NULL);
    Ns_Log(Dev, "config: %s:%s value=%s (string)",
           section ? section : "", key, value ? value : "");

    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetValueExact --
 *
 *      Case-sensitive version of Ns_ConfigGetValue
 *
 * Results:
 *      See Ns_ConfigGetValue
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConfigGetValueExact(CONST char *section, CONST char *key)
{
    char *value;

    value = ConfigGet(section, key, 1, NULL);
    Ns_Log(Dev, "config: %s:%s value=%s (string, exact match)",
           section ? section : "", key, value ? value : "");

    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetInt --
 *
 *      Fetch integer config values
 *
 * Results:
 *      NS_TRUE if it found an integer value; otherwise, it returns
 *      NS_FALSE and sets the value to 0
 *
 * Side effects:
 *      The integer value is returned by reference
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConfigGetInt(CONST char *section, CONST char *key, int *valuePtr)
{
    CONST char *s;
    int found;

    s = ConfigGet(section, key, 0, NULL);
    if (s != NULL && Ns_StrToInt(s, valuePtr) == NS_OK) {
        Ns_Log(Dev, "config: %s:%s value=%d min=%d max=%d (int)",
               section ? section : "", key, *valuePtr, INT_MIN, INT_MAX);
        found = NS_TRUE;
    } else {
        Ns_Log(Dev, "config: %s:%s value=(null) min=%d max=%d (int)",
               section ? section : "", key, INT_MIN, INT_MAX);
        *valuePtr = 0;
        found = NS_FALSE;
    }
    return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetInt64 --
 *
 *      Like Ns_ConfigGetInt, but with INT64 data instead of
 *      system-native int types.
 *
 * Results:
 *      See Ns_ConfigGetInt
 *
 * Side effects:
 *      See Ns_ConfigGetInt
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConfigGetInt64(CONST char *section, CONST char *key, int64_t *valuePtr)
{
    char *s;

    s = Ns_ConfigGetValue(section, key);
    if (s == NULL || sscanf(s, "%" PRId64, valuePtr) != 1) {
        return NS_FALSE;
    }
    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetBool --
 *
 *      Get a boolean config value. There are many ways to represent
 *      a boolean value.
 *
 * Results:
 *      NS_TRUE/NS_FALSE
 *
 * Side effects:
 *      The boolean value is returned by reference
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConfigGetBool(CONST char *section, CONST char *key, int *valuePtr)
{
    CONST char *s;
    int found = NS_FALSE;

    s = ConfigGet(section, key, 0, NULL);
    if (s != NULL && ToBool(s, valuePtr)) {
        found = NS_TRUE;
    }
    Ns_Log(Dev, "config: %s:%s value=%s (bool)",
           section ? section : "", key,
           found ? (*valuePtr ? "true" : "false") : "(null)");

    return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetPath --
 *
 *      Get the full name of a config file section if it exists.
 *
 * Results:
 *      A pointer to an ASCIIZ string of the full path name, or NULL
 *      if that path is not in the config file.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConfigGetPath(CONST char *server, CONST char *module, ...)
{
    va_list         ap;
    char           *s;
    Ns_DString      ds;
    Ns_Set         *set;

    Ns_DStringInit(&ds);
    Ns_DStringAppend(&ds, "ns");
    if (server != NULL) {
        Ns_DStringVarAppend(&ds, "/server/", server, NULL);
    }
    if (module != NULL) {
        Ns_DStringVarAppend(&ds, "/module/", module, NULL);
    }
    va_start(ap, module);
    while ((s = va_arg(ap, char *)) != NULL) {
        Ns_DStringAppend(&ds, "/");
        while (*s != '\0' && ISSLASH(*s)) {
            ++s;
        }
        Ns_DStringAppend(&ds, s);
        while (ISSLASH(ds.string[ds.length - 1])) {
            ds.string[--ds.length] = '\0';
        }
    }
    va_end(ap);
    Ns_Log(Dev, "config section: %s", Ns_DStringValue(&ds));

    set = Ns_ConfigCreateSection(ds.string);
    Ns_DStringFree(&ds);

    return (set ? Ns_SetName(set) : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetSections --
 *
 *      Return a malloc'ed, NULL-terminated array of sets, each
 *      corresponding to a config section.
 *
 * Results:
 *      An array of sets.
 *
 * Side effects:
 *      The result is malloc'ed memory.
 *
 *----------------------------------------------------------------------
 */

Ns_Set **
Ns_ConfigGetSections(void)
{
    Ns_Set        **sets;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    int             n;

    n = nsconf.sections.numEntries + 1;
    sets = ns_malloc(sizeof(Ns_Set *) * n);
    n = 0;
    hPtr = Tcl_FirstHashEntry(&nsconf.sections, &search);
    while (hPtr != NULL) {
        sets[n++] = Tcl_GetHashValue(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    sets[n] = NULL;

    return sets;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigGetSection --
 *
 *      Return the Ns_Set of a config section called section.
 *
 * Results:
 *      An Ns_Set containing the section's parameters, or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_ConfigGetSection(CONST char *section)
{
    return (section ? GetSection(section, 0) : NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConfigCreateSection --
 *
 *      Return the Ns_Set of a config section called section.
 *
 * Results:
 *      An Ns_Set containing the section's parameters, or NULL.
 *
 * Side effects:
 *      New section can be created if it does not exist
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_ConfigCreateSection(CONST char *section)
{
    int create = Ns_InfoStarted() ? 0 : 1;
    return (section ? GetSection(section, create) : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetVersion --
 *
 *      Get the major, minor, and patchlevel version numbers and
 *      the release type. A patch is a release type NS_FINAL_RELEASE
 *      with a patchLevel > 0.
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
Ns_GetVersion(int *majorV, int *minorV, int *patchLevelV, int *type)
{
    if (majorV != NULL) {
        *majorV = NS_MAJOR_VERSION;
    }
    if (minorV != NULL) {
        *minorV = NS_MINOR_VERSION;
    }
    if (patchLevelV != NULL) {
        *patchLevelV = NS_RELEASE_SERIAL;
    }
    if (type != NULL) {
        *type = NS_RELEASE_LEVEL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsConfigRead --
 *
 *      Read a config file at startup.
 *
 * Results:
 *      Pointer to the config buffer in an ns_malloc'ed string.
 *
 * Side Effects:
 *      Server aborts if the file cannot be read for any reason.
 *
 *---------------------------------------------------------------------
 */

char *
NsConfigRead(CONST char *file)
{
    Tcl_Channel  chan = NULL;
    Tcl_Obj     *buf = NULL;
    char        *call = "open", *data, *conf = NULL;
    int          length;

    if (file == NULL || *file == '\0') {
        goto err;
    }

    /*
     * Open the channel for reading the config file
     */

    chan = Tcl_OpenFileChannel(NULL, file, "r", 0);
    if (chan == NULL) {
        goto err;
    }

    /*
     * Slurp entire file in memory
     */

    buf = Tcl_NewObj();
    Tcl_IncrRefCount(buf);
    if (Tcl_ReadChars(chan, buf, -1, 0) == -1) {
        call = "read";
        goto err;
    }

    Tcl_Close(NULL, chan);
    data = Tcl_GetStringFromObj(buf, &length);
    conf = strcpy(ns_malloc((size_t)length + 1U), data);
    Tcl_DecrRefCount(buf);

    return conf;

 err:
    if (chan) {
        Tcl_Close(NULL, chan);
    }
    if (buf) {
        Tcl_DecrRefCount(buf);
    }
    Ns_Fatal("config: can't %s config file '%s': '%s' %s",
             call,
             file,
             strerror(Tcl_GetErrno()),
             file ? "" : "(Did you pass -t option to nsd?)");

    return NULL; /* Keep the compiler happy */
}


/*
 *----------------------------------------------------------------------
 *
 * NsConfigEval --
 *
 *      Eval config script in a startup Tcl interp.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Various variables in the configInterp will be set as well as
 *      the sundry configuration hashes.
 *
 *---------------------------------------------------------------------
 */

void
NsConfigEval(const char *config, int argc, char *const *argv, int optind)
{
    Tcl_Interp *interp;
    Ns_Set     *set;
    int i;

    /*
     * Create an interp with a few config-related commands.
     */

    set = NULL;
    interp = Ns_TclCreateInterp();
    Tcl_CreateCommand(interp, "ns_section", SectionCmd, &set, NULL);
    Tcl_CreateCommand(interp, "ns_param", ParamCmd, &set, NULL);
    for (i = 0; argv[i] != NULL; ++i) {
        Tcl_SetVar(interp, "argv", argv[i], TCL_APPEND_VALUE|TCL_LIST_ELEMENT|TCL_GLOBAL_ONLY);
    }
    Tcl_SetVar2Ex(interp, "argc", NULL, Tcl_NewIntObj(argc), TCL_GLOBAL_ONLY);
    Tcl_SetVar2Ex(interp, "optind", NULL, Tcl_NewIntObj(optind), TCL_GLOBAL_ONLY);
    if (Tcl_Eval(interp, config) != TCL_OK) {
        Ns_TclLogError(interp);
        Ns_Fatal("config error");
    }
    Ns_TclDestroyInterp(interp);
}


/*
 *----------------------------------------------------------------------
 *
 * ParamCmd --
 *
 *      Add a single entry to the current section of the config.  This
 *      command may only be run from within an ns_section.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ParamCmd(ClientData clientData, Tcl_Interp *interp, int argc, CONST84 char *argv[])
{
    Ns_Set *set;

    if (argc != 3) {
        Tcl_AppendResult(interp, "wrong # args: should be \"",
                         argv[0], " key value", NULL);
        return TCL_ERROR;
    }
    set = *((Ns_Set **) clientData);
    if (set == NULL) {
        Tcl_AppendResult(interp, argv[0],
                         " not preceded by an ns_section command.", NULL);
        return TCL_ERROR;
    }
    Ns_SetPut(set, argv[1], argv[2]);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * SectionCmd --
 *
 *      This creates a new config section and sets a shared variable
 *      to point at a newly-allocated set for holding config data.
 *      ns_param stores config data in the set.
 *
 * Results:
 *      Standard tcl result.
 *
 * Side effects:
 *      Section set is created (if necessary).
 *
 *----------------------------------------------------------------------
 */

static int
SectionCmd(ClientData clientData, Tcl_Interp *interp, int argc, CONST84 char *argv[])
{
    Ns_Set  **set;

    if (argc != 2) {
        Tcl_AppendResult(interp, "wrong # args: should be \"",
                         argv[0], " sectionname", NULL);
        return TCL_ERROR;
    }
    set = (Ns_Set **) clientData;
    *set = GetSection(argv[1], 1);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ConfigGet --
 *
 *      Return the value for key in the config section.
 *
 * Results:
 *      Pointer to value, or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
ConfigGet(CONST char *section, CONST char *key, int exact, CONST char *defstr)
{
    char           *s;

    s = NULL;
    if (section != NULL && key != NULL) {
	Ns_Set  *set = GetSection(section, 0);
        if (set != NULL) {
            int  i;
            if (exact) {
                i = Ns_SetFind(set, key);
            } else {
                i = Ns_SetIFind(set, key);
            }
            if (i >= 0) {
                s = Ns_SetValue(set, i);
            } else {
                i = Ns_SetPut(set, key, defstr);
                if (defstr) {
                    s = Ns_SetValue(set, i);
                }
            }
        }
    }
    return s;
}


/*
 *----------------------------------------------------------------------
 *
 * GetSection --
 *
 *      Creates and/or gets a config section.
 *
 * Results:
 *      Pointer to new or existing Ns_Set for given section.
 *
 * Side effects:
 *      Section set created (if necessary).
 *
 *----------------------------------------------------------------------
 */

static Ns_Set *
GetSection(CONST char *section, int create)
{
    Ns_Set        *set;
    Tcl_HashEntry *hPtr;
    Ns_DString     ds;
    int            isNew;
    CONST char    *p;
    char          *s;

    /*
     * Clean up section name to all lowercase, trimming space
     * and swapping silly backslashes.
     */

    Ns_DStringInit(&ds);
    p = section;
    while (isspace(UCHAR(*p))) {
        ++p;
    }
    Ns_DStringAppend(&ds, p);
    s = ds.string;
    while (unlikely(*s != '\0')) {
	if (unlikely(*s == '\\')) {
            *s = '/';
        } else if (unlikely(isupper(UCHAR(*s)))) {
            *s = tolower(UCHAR(*s));
        }
        ++s;
    }
    while (--s > ds.string && (unlikely(isspace(UCHAR(*s))))) {
        *s = '\0';
    }
    section = ds.string;

    /*
     * Return config set, creating if necessary.
     */

    set = NULL;
    if (likely(create != 1)) {
        hPtr = Tcl_FindHashEntry(&nsconf.sections, section);
    } else {
        hPtr = Tcl_CreateHashEntry(&nsconf.sections, section, &isNew);
        if (isNew) {
            set = Ns_SetCreate(section);
	    Tcl_SetHashValue(hPtr, set);
        }
    }
    if (hPtr != NULL) {
        set = Tcl_GetHashValue(hPtr);
    }
    Ns_DStringFree(&ds);

    return set;
}


/*
 *----------------------------------------------------------------------
 *
 * ToBool --
 *
 *      Interpret value as a boolean.  There are many ways to represent
 *      a boolean value.
 *
 * Results:
 *      NS_TRUE if value converted to boolean, NS_FALSE otherwise.
 *
 * Side effects:
 *      The boolean value is returned by reference.
 *
 *----------------------------------------------------------------------
 */

static int
ToBool(CONST char *value, int *valuePtr)
{
    int boolValue;

    if (STREQ(value, "1")
        || STRIEQ(value, "y")
        || STRIEQ(value, "yes")
        || STRIEQ(value, "on")
        || STRIEQ(value, "t")
        || STRIEQ(value, "true")) {

        boolValue = NS_TRUE;
    } else if (STREQ(value, "0")
               || STRIEQ(value, "n")
               || STRIEQ(value, "no")
               || STRIEQ(value, "off")
               || STRIEQ(value, "f")
               || STRIEQ(value, "false")) {

        boolValue = NS_FALSE;
    } else if (Ns_StrToInt(value, &boolValue) != NS_OK) {
        return NS_FALSE;
    }
    *valuePtr = boolValue ? NS_TRUE : NS_FALSE;

    return NS_TRUE;
}
