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
 *      Implement the "ns_env" command.
 */

#include "nsd.h"

#ifdef HAVE__NSGETENVIRON
#include <crt_externs.h>
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

static Ns_Mutex lock;


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
 *      Implements the ns_env command.  No attempt is made to avoid the
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
NsTclEnvObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char  *name, *value;
    char       **envp;
    int          status, i, opt;
    Tcl_Obj     *result;

    static const char *opts[] = {
        "exists", "names", "get", "set", "unset", NULL
    };
    enum {
        IExistsIdx, INamesIdx, IGetIdx, ISetIdx, IUnsetIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "command", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    status = TCL_ERROR;
    Ns_MutexLock(&lock);

    switch (opt) {
    case IExistsIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "name");
            goto done;
        }
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(getenv(Tcl_GetString(objv[2])) != NULL ? 1 : 0));
        break;

    case INamesIdx:
        envp = Ns_GetEnviron();
        result = Tcl_GetObjResult(interp);
        for (i = 0; envp[i] != NULL; ++i) {
            name = envp[i];
            value = strchr(name, '=');
            Tcl_ListObjAppendElement(interp, result,
		     Tcl_NewStringObj(name, (value != NULL) ? (int)(value - name) : -1));
        }
        break;

    case ISetIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "name value");
            goto done;
        }
        if (PutEnv(interp, Tcl_GetString(objv[2]), Tcl_GetString(objv[3]))
            != NS_OK) {
            goto done;
        }
        break;

    case IGetIdx:
    case IUnsetIdx:
	if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-nocomplain? name");
            goto done;
        }

        if (objc == 4) {
	    const char *arg = Tcl_GetString(objv[2]);
	    if (!STREQ(arg, "-nocomplain")) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-nocomplain? name");
		goto done;
	    }
        }
        name = Tcl_GetString(objv[2]);
        value = getenv(name);
        if (value == NULL && objc != 4) {
            Tcl_SetResult(interp, "no such environment variable", TCL_STATIC);
            goto done;
        }
        if (opt == IUnsetIdx && PutEnv(interp, name, NULL) != NS_OK) {
            goto done;
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(value, -1));
        }
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }
    status = TCL_OK;

 done:
    Ns_MutexUnlock(&lock);

    return status;
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

#ifdef HAVE_UNSETENV
    if (value == NULL) {
        unsetenv(name);
        return TCL_OK;
    }
#endif

    len = nameLength = strlen(name);
    if (value != NULL) {
        valueLength = strlen(value);
        len += valueLength + 1u;
    } else {
        len += 1u;
    }
    /* 
     * Use malloc() directly (and not ns_malloc())
     * as putenv() expects. 
     */
    s = malloc(len + 1u);
    if (s == NULL) {
        Tcl_SetResult(interp, "could not allocate memory for new env entry",
                      TCL_STATIC);
        return TCL_ERROR;
    }

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
    *(s + nameLength + 1u) = '\0';

    if (value != NULL) {
        strncat(s + nameLength + 1, value, valueLength);
    }

    if (putenv(s) != 0) {
        Tcl_AppendResult(interp, "could not put environment entry \"",
                         s, "\": ", Tcl_PosixError(interp), NULL);
        free(s);
        return TCL_ERROR;
    }
#if 0    
    if (value == NULL) {
        strncpy(s, "=", 2u);
        putenv(s);
    }
#endif

    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
