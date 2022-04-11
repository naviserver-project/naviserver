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
 * tclenv.c --
 *
 *      Implements "ns_env".
 */

#include "nsd.h"

#ifdef HAVE__NSGETENVIRON
# include <crt_externs.h>
#elif !defined(_WIN32)
extern char **environ;
#endif

/*
 * Local functions defined in this file.
 */

static int PutEnv(Tcl_Interp *interp, const char *name, const char *value);

/*
 * Loca variables defined in this file.
 */

static Ns_Mutex lock = NULL;


/*
 *----------------------------------------------------------------------
 *
 * NsInitTclEnv --
 *
 *      Global initialization for tasks.
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
NsInitTclEnv(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        Ns_MutexInit(&lock);
        Ns_MutexSetName(&lock, "ns:env");
        initialized = NS_TRUE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_GetEnviron --
 *
 *      Return the environment vector.
 *
 * Results:
 *      Pointer to environment.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char **
Ns_GetEnviron(void)
{
    char **envp;

#ifdef HAVE__NSGETENVIRON
    envp = *_NSGetEnviron();
#else
    envp = environ;
#endif
    return envp;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CopyEnviron --
 *
 *      Copy the environment to the given dstring along with
 *      an argv vector.
 *
 * Results:
 *      Pointer to dsPtr->string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char **
Ns_CopyEnviron(Ns_DString *dsPtr)
{
    char *const*envp;
    int i;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MutexLock(&lock);
    envp = Ns_GetEnviron();
    for (i = 0;  envp[i] != NULL; ++i) {
        Ns_DStringAppendArg(dsPtr, envp[i]);
    }
    Ns_MutexUnlock(&lock);

    return Ns_DStringAppendArgv(dsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclEnvObjCmd --
 *
 *      Implements "ns_env".  No attempt is made to avoid the
 *      race condition between finding a variable and using it as it is
 *      assumed the environment would only be modified, if ever, at
 *      startup.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Environment variables may be updated.
 *
 *----------------------------------------------------------------------
 */

int
NsTclEnvObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int                      result, opt;
    static const char *const opts[] = {
        "exists", "names", "get", "set", "unset", NULL
    };
    enum {
        IExistsIdx, INamesIdx, IGetIdx, ISetIdx, IUnsetIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args ...?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "command", 0,
                            &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        const char  *name, *value;
        char        *const *envp;
        Tcl_Obj     *resultObj;
        int          i;

        result = TCL_OK;
        Ns_MutexLock(&lock);

        switch (opt) {
        case IExistsIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "name");
                result = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, Tcl_NewBooleanObj((getenv(Tcl_GetString(objv[2])) != NULL) ? 1 : 0));
            }
            break;

        case INamesIdx:
            envp = Ns_GetEnviron();
            resultObj = Tcl_GetObjResult(interp);
            for (i = 0; envp[i] != NULL; ++i) {
                Tcl_Obj *obj;

                name = envp[i];
                value = strchr(name, INTCHAR('='));
                obj = Tcl_NewStringObj(name, (value != NULL) ? (int)(value - name) : -1);
                if (Tcl_ListObjAppendElement(interp, resultObj, obj) != TCL_OK) {
                    result = TCL_ERROR;
                    break;
                }
            }
            break;

        case ISetIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "name value");
                result = TCL_ERROR;

            } else if (PutEnv(interp, Tcl_GetString(objv[2]), Tcl_GetString(objv[3])) != TCL_OK) {
                result = TCL_ERROR;
            }
            break;

        case IGetIdx:
        case IUnsetIdx:
            if (objc != 3 && objc != 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "?-nocomplain? name");
                result = TCL_ERROR;

            } else if (objc == 4) {
                const char *arg = Tcl_GetString(objv[2]);

                if (!STREQ(arg, "-nocomplain")) {
                    Tcl_WrongNumArgs(interp, 2, objv, "?-nocomplain? name");
                    result = TCL_ERROR;
                }
            }

            if (result == TCL_OK) {
                name = Tcl_GetString(objv[2]);
                value = getenv(name);
                if (value == NULL && objc != 4) {
                    Ns_TclPrintfResult(interp, "no such environment variable: %s", name);
                    result = TCL_ERROR;

                } else if ((opt == IUnsetIdx) && (PutEnv(interp, name, NULL) != TCL_OK)) {
                    result = TCL_ERROR;

                } else {
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(value, -1));
                }
            }
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }

        Ns_MutexUnlock(&lock);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * PutEnv --
 *
 *      NsTclEnvObjCmd helper routine to update an environment variable.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Environment variable is set.
 *
 *----------------------------------------------------------------------
 */

static int
PutEnv(Tcl_Interp *interp, const char *name, const char *value)
{
    char   *s;
    size_t  len, nameLength, valueLength;
    int     result = TCL_OK;

#ifdef HAVE_UNSETENV
    if (value == NULL) {
        unsetenv(name);
        return result;
    }
#endif

    /*
     * In case we have no unsetenv(), we have to deal with the value==NULL
     * case here. If the value is not NULL, valueLength contains the
     * terminating NUL character.
     */
    len = nameLength = strlen(name);
    if (value != NULL) {
        valueLength = strlen(value) + 1;
        len += valueLength + 1u;
    } else {
        len += 1u;
        valueLength = 0u;
    }

    /*
     * Use malloc() directly (and not ns_malloc())
     * as putenv() expects.
     */
    s = malloc(len + 1u);
    if (s == NULL) {
        Ns_TclPrintfResult(interp, "could not allocate memory for new env entry");
        result = TCL_ERROR;
    } else {

        /*
         * This complication for value == NULL below is needed on
         * some platforms (Solaris) which do not have unsetenv()
         * and are picky if we try to pass a value to putenv not
         * conforming to the "name=value" format.
         *
         * This trick will of course work only for platforms which
         * conform to Single Unix Spec and actually uses the storage
         * passed to putenv() to hold the environ entry.
         * However, there are some libc implementations (notably
         * recent BSDs) that do not obey SUS but copy the presented
         * string. This method fails on such platforms.
         */
        memcpy(s, name, nameLength);
        *(s + nameLength) = '=';

        if (valueLength != 0u) {
            /*
             * Copy the value including the terminatig NUL character.
             */
            memcpy(s + nameLength + 1, value, valueLength);
        } else {
            *(s + nameLength + 1u) = '\0';
        }

        if (putenv(s) != 0) {
            Ns_TclPrintfResult(interp, "could not put environment entry \"%s\": %s",
                               s, Tcl_PosixError(interp));
            free(s);
            result = TCL_ERROR;
        }
    }

    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
