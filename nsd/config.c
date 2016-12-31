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

static Tcl_ObjCmdProc SectionObjCmd;
static Tcl_ObjCmdProc ParamObjCmd;

static Ns_Set* GetSection(const char *section, bool create)
    NS_GNUC_NONNULL(1);

static const char* ConfigGet(const char *section, const char *key, int exact, const char *defstr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static bool ToBool(const char *value, bool *valuePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);




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

const char *
Ns_ConfigString(const char *section, const char *key, const char *def)
{
    const char *value;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    
    value = ConfigGet(section, key, 0, def);
    Ns_Log(Dev, "config: %s:%s value=\"%s\" default=\"%s\" (string)", 
	   section, key, 
	   (value != NULL) ? value : "", 
	   (def != NULL) ? def : "");

    return (value != NULL) ? value : def;
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

bool
Ns_ConfigBool(const char *section, const char *key, bool def)
{
    const char *s;
    bool value = NS_FALSE, found = NS_FALSE;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    s = ConfigGet(section, key, 0, def ? "true" : "false");
    if (s != NULL && ToBool(s, &value)) {
        found = NS_TRUE;
    }
    Ns_Log(Dev, "config: %s:%s value=%s default=%s (bool)",
           section, key,
           found ? (value ? "true" : "false") : "(null)",
	   def   ? "true" : "false");

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

bool
Ns_ConfigFlag(const char *section, const char *key, unsigned int flag, int def,
              unsigned int *flagsPtr)
{
    const char *s;
    bool value = NS_FALSE, found = NS_FALSE;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    s = ConfigGet(section, key, 0, (def != 0) ? "true" : "false");
    if (s != NULL && ToBool(s, &value)) {
        found = NS_TRUE;
    }

    Ns_Log(Dev, "config: %s:%s value=%u default=%u (flag)", 
	   section, key, 
	   value ? flag : 0u, 
	   (def != 0) ? flag : 0u);

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
Ns_ConfigInt(const char *section, const char *key, int def)
{
    return Ns_ConfigIntRange(section, key, def, INT_MIN, INT_MAX);
}

int
Ns_ConfigIntRange(const char *section, const char *key, int def,
                  int min, int max)
{
    const char *s;
    char defstr[TCL_INTEGER_SPACE];
    int value;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    snprintf(defstr, sizeof(defstr), "%d", def);
    s = ConfigGet(section, key, 0, defstr);
    if (s != NULL && Ns_StrToInt(s, &value) == NS_OK) {
        Ns_Log(Dev, "config: %s:%s value=%d min=%d max=%d default=%d (int)", 
	       section, key, value, min, max, def);
    } else {
        Ns_Log(Dev, "config: %s:%s value=(null) min=%d max=%d default=%d (int)", 
	       section, key, min, max, def);
        value = def;
    }
    if (value < min) {
        Ns_Log(Warning, "config: %s:%s value=%d, rounded up to %d", 
	       section, key, value, min);
        value = min;
    }
    if (value > max) {
        Ns_Log(Warning, "config: %s:%s value=%d, rounded down to %d", 
	       section, key, value, max);
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
#ifdef TCL_WIDE_INT_IS_LONG
# define WIDE_INT_MAX (LONG_MAX)
# define WIDE_INT_MIN (LONG_MIN)
#else
# define WIDE_INT_MAX (LLONG_MAX)
# define WIDE_INT_MIN (LLONG_MIN)
#endif

Tcl_WideInt
Ns_ConfigWideInt(const char *section, const char *key, Tcl_WideInt def)
{
    return Ns_ConfigWideIntRange(section, key, def, WIDE_INT_MIN, WIDE_INT_MAX);
}

Tcl_WideInt
Ns_ConfigWideIntRange(const char *section, const char *key, Tcl_WideInt def,
                  Tcl_WideInt min, Tcl_WideInt max)
{
    const char *s;
    char defstr[TCL_INTEGER_SPACE];
    Tcl_WideInt value;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    snprintf(defstr, sizeof(defstr), "%" TCL_LL_MODIFIER "d", def);
    s = ConfigGet(section, key, 0, defstr);
    if (s != NULL && Ns_StrToWideInt(s, &value) == NS_OK) {
        Ns_Log(Dev, "config: %s:%s value=%" TCL_LL_MODIFIER "d min=%" TCL_LL_MODIFIER 
	       "d max=%" TCL_LL_MODIFIER "d default=%" TCL_LL_MODIFIER "d (wide int)", 
	       section, key, value, min, max, def);
    } else {
	Ns_Log(Dev, "config: %s:%s value=(null) min=%" TCL_LL_MODIFIER "d max=%" TCL_LL_MODIFIER 
	       "d default=%" TCL_LL_MODIFIER "d (wide int)", 
	       section, key, min, max, def);
        value = def;
    }
    if (value < min) {
        Ns_Log(Warning, "config: %s:%s value=%" TCL_LL_MODIFIER "d, rounded up to %" 
	       TCL_LL_MODIFIER "d", section, key, value, min);
        value = min;
    }
    if (value > max) {
        Ns_Log(Warning, "config: %s:%s value=%" TCL_LL_MODIFIER "d, rounded down to %" 
	       TCL_LL_MODIFIER "d", section, key, value, max);
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
 *      char ptr to a value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConfigGetValue(const char *section, const char *key)
{
    const char *value;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    
    value = ConfigGet(section, key, 0, NULL);
    Ns_Log(Dev, "config: %s:%s value=%s (string)", 
	   section, key, (value != NULL) ? value : "");

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

const char *
Ns_ConfigGetValueExact(const char *section, const char *key)
{
    const char *value;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    
    value = ConfigGet(section, key, 1, NULL);
    Ns_Log(Dev, "config: %s:%s value=%s (string, exact match)", 
	   section, key, 
	   (value != NULL) ? value : "");

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

bool
Ns_ConfigGetInt(const char *section, const char *key, int *valuePtr)
{
    const char *s;
    bool found;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    s = ConfigGet(section, key, 0, NULL);
    if (s != NULL && Ns_StrToInt(s, valuePtr) == NS_OK) {
        Ns_Log(Dev, "config: %s:%s value=%d min=%d max=%d (int)", 
	       section, key, *valuePtr, INT_MIN, INT_MAX);
        found = NS_TRUE;
    } else {
        Ns_Log(Dev, "config: %s:%s value=(null) min=%d max=%d (int)", 
	       section, key, INT_MIN, INT_MAX);
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

bool
Ns_ConfigGetInt64(const char *section, const char *key, int64_t *valuePtr)
{
    const char *s;
    bool        success = NS_TRUE;

    s = Ns_ConfigGetValue(section, key);
    if (s == NULL || sscanf(s, "%24" SCNd64, valuePtr) != 1) {
        success = NS_FALSE;
    }
    return success;
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
 *      NS_TRUE/NS_FALSE when parameter was found
 *
 * Side effects:
 *      The boolean value is returned by reference
 *
 *----------------------------------------------------------------------
 */

bool
Ns_ConfigGetBool(const char *section, const char *key, bool *valuePtr)
{
    const char *s;
    bool found = NS_FALSE;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(valuePtr != NULL);

    s = ConfigGet(section, key, 0, NULL);
    if (s != NULL && ToBool(s, valuePtr)) {
        found = NS_TRUE;
    }
    Ns_Log(Dev, "config: %s:%s value=%s (bool)", 
	   section, key, found ? (*valuePtr ? "true" : "false") : "(null)");

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

const char *
Ns_ConfigGetPath(const char *server, const char *module, ...)
{
    va_list         ap;
    const char     *s;
    Ns_DString      ds;
    const Ns_Set   *set;

    Ns_DStringInit(&ds);
    Ns_DStringAppend(&ds, "ns");
    if (server != NULL) {
        Ns_DStringVarAppend(&ds, "/server/", server, (char *)0);
    }
    if (module != NULL) {
        Ns_DStringVarAppend(&ds, "/module/", module, (char *)0);
    }
    va_start(ap, module);
    for (s = va_arg(ap, char *); s != NULL; s = va_arg(ap, char *)) {
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

    return (set != NULL) ? Ns_SetName(set) : NULL;
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
    Ns_Set             **sets;
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;
    int                  n;

    n = nsconf.sections.numEntries + 1;
    sets = ns_malloc(sizeof(Ns_Set *) * (size_t)n);
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
Ns_ConfigGetSection(const char *section)
{
    return GetSection(section, NS_FALSE);
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
Ns_ConfigCreateSection(const char *section)
{
    bool create = Ns_InfoStarted() ? NS_FALSE : NS_TRUE;
    return GetSection(section, create);
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

const char *
NsConfigRead(const char *file)
{
    Tcl_Channel  chan;
    Tcl_Obj     *buf;
    const char  *call = "open", *conf = NULL;

    NS_NONNULL_ASSERT(file != NULL);

    /*
     * Open the channel for reading the config file
     */
    chan = Tcl_OpenFileChannel(NULL, file, "r", 0);
    if (chan == NULL) {
        buf = NULL;

    } else {

        /*
         * Slurp entire file into memory
         */
        buf = Tcl_NewObj();
        Tcl_IncrRefCount(buf);
        if (Tcl_ReadChars(chan, buf, -1, 0) == -1) {
            call = "read";

        } else {
            int         length;
            const char *data = Tcl_GetStringFromObj(buf, &length);
           
            conf = ns_strncopy(data, length);
        }
    }
    
    if (chan != NULL) {
        (void) Tcl_Close(NULL, chan);
    }
    if (buf != NULL) {
        Tcl_DecrRefCount(buf);
    }
    if (conf == NULL) {
        Ns_Fatal("config: can't %s config file '%s': '%s'",
                 call, file, strerror(Tcl_GetErrno()));
    }

    return conf; 
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
    Tcl_Interp   *interp;
    const Ns_Set *set;
    int i;

    NS_NONNULL_ASSERT(config != NULL);

    /*
     * Create an interp with a few config-related commands.
     */

    set = NULL;
    interp = Ns_TclCreateInterp();
    (void)Tcl_CreateObjCommand(interp, "ns_section", SectionObjCmd, &set, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_param", ParamObjCmd, &set, NULL);
    for (i = 0; argv[i] != NULL; ++i) {
        (void) Tcl_SetVar(interp, "argv", argv[i], TCL_APPEND_VALUE|TCL_LIST_ELEMENT|TCL_GLOBAL_ONLY);
    }
    (void) Tcl_SetVar2Ex(interp, "argc", NULL, Tcl_NewIntObj(argc), TCL_GLOBAL_ONLY);
    (void) Tcl_SetVar2Ex(interp, "optind", NULL, Tcl_NewIntObj(optind), TCL_GLOBAL_ONLY);
    if (Tcl_Eval(interp, config) != TCL_OK) {
        (void) Ns_TclLogErrorInfo(interp, "\n(context: config eval)");
        Ns_Fatal("config error");
    }
    Ns_TclDestroyInterp(interp);
}


/*
 *----------------------------------------------------------------------
 *
 * ParamObjCmd --
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
ParamObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;
    const char *paramName = NULL, *paramValue = NULL;
    Ns_ObjvSpec args[] = {
        {"name",  Ns_ObjvString,  &paramName, NULL},
        {"value", Ns_ObjvString,  &paramValue, NULL},        
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK)) {
        result = TCL_ERROR;
        
    } else {
        Ns_Set *set = *((Ns_Set **) clientData);
        
        if (likely(set != NULL)) {
            (void)Ns_SetPut(set, paramName, paramValue);
        } else {
            Ns_TclPrintfResult(interp, "parameter %s not preceded by an ns_section command.", paramName);
            result = TCL_ERROR;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SectionObjCmd --
 *
 *      This creates a new config section and sets a shared variable
 *      to point at a newly-allocated set for holding config data.
 *      ns_param stores config data in the set.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Section set is created (if necessary).
 *
 *----------------------------------------------------------------------
 */

static int
SectionObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;
    const char *sectionName = NULL;
    Ns_ObjvSpec args[] = {
        {"sectionname", Ns_ObjvString,  &sectionName, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK)) {
        result = TCL_ERROR;
        
    } else {
        Ns_Set  **set = (Ns_Set **) clientData;
        
        *set = GetSection(sectionName, NS_TRUE);
    }

    return result;
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

static const char *
ConfigGet(const char *section, const char *key, int exact, const char *defstr)
{
    const char *s;
    Ns_Set     *set;

    NS_NONNULL_ASSERT(section != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    s = NULL;
    set = GetSection(section, NS_FALSE);

    if (set != NULL) {
	int  i;
	if (exact != 0) {
	    i = Ns_SetFind(set, key);
	} else {
	    i = Ns_SetIFind(set, key);
	}
	if (i >= 0) {
	    s = Ns_SetValue(set, i);
	} else {
	    i = (int)Ns_SetPut(set, key, defstr);
	    if (defstr != NULL) {
		s = Ns_SetValue(set, i);
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
GetSection(const char *section, bool create)
{
    Ns_Set        *set;
    Tcl_HashEntry *hPtr;
    Ns_DString     ds;
    int            isNew;
    const char    *p;
    char          *s;

    NS_NONNULL_ASSERT(section != NULL);

    /*
     * Clean up section name to all lowercase, trimming space
     * and swapping silly backslashes.
     */

    Ns_DStringInit(&ds);
    p = section;
    while (CHARTYPE(space, *p) != 0) {
        ++p;
    }
    Ns_DStringAppend(&ds, p);
    s = ds.string;
    while (likely(*s != '\0')) {
	if (unlikely(*s == '\\')) {
            *s = '/';
        } else if (unlikely(CHARTYPE(upper, *s) != 0)) {
            *s = CHARCONV(lower, *s);
        }
        ++s;
    }
    while (--s > ds.string && (unlikely(CHARTYPE(space, *s) != 0))) {
        *s = '\0';
    }
    section = ds.string;

    /*
     * Return config set, creating if necessary.
     */

    set = NULL;
    if (likely(!create)) {
        hPtr = Tcl_FindHashEntry(&nsconf.sections, section);
    } else {
        hPtr = Tcl_CreateHashEntry(&nsconf.sections, section, &isNew);
        if (isNew != 0) {
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

static bool
ToBool(const char *value, bool *valuePtr)
{
    int boolValue;
    bool success = NS_TRUE;

    NS_NONNULL_ASSERT(value != NULL);
    NS_NONNULL_ASSERT(valuePtr != NULL);

    if (STREQ(value, "1")
        || STRIEQ(value, "y")
        || STRIEQ(value, "yes")
        || STRIEQ(value, "on")
        || STRIEQ(value, "t")
        || STRIEQ(value, "true")) {

        boolValue = (int)NS_TRUE;
    } else if (STREQ(value, "0")
               || STRIEQ(value, "n")
               || STRIEQ(value, "no")
               || STRIEQ(value, "off")
               || STRIEQ(value, "f")
               || STRIEQ(value, "false")) {

        boolValue = (int)NS_FALSE;
    } else if (Ns_StrToInt(value, &boolValue) != NS_OK) {
        success = NS_FALSE;
    }
    if (success) {
        *valuePtr = (boolValue != (int)NS_FALSE) ? NS_TRUE : NS_FALSE;
    }

    return success;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
