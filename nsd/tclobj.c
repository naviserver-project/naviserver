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
 * tclobj.c --
 *
 *      Helper routines for managing Tcl object types.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");



/*
 *----------------------------------------------------------------------
 *
 * Ns_TclResetObjType --
 *
 *      Reset the given Tcl_Obj type, freeing any type specific
 *      internal representation.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on object type.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclResetObjType(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr)
{
    Tcl_ObjType *typePtr = objPtr->typePtr;

    if (typePtr != NULL && typePtr->freeIntRepProc != NULL) {
        (*typePtr->freeIntRepProc)(objPtr);
    }
    objPtr->typePtr = newTypePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetTwoPtrValue --
 *
 *      Reset the given objects type and values, freeing any existing
 *      internal rep.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on object type.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclSetTwoPtrValue(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr,
                     void *ptr1, void *ptr2)
{
    Ns_TclResetObjType(objPtr, newTypePtr);
    objPtr->internalRep.twoPtrValue.ptr1 = ptr1;
    objPtr->internalRep.twoPtrValue.ptr2 = ptr2;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetOtherValuePtr --
 *
 *      Reset the given objects type and value, freeing any existing
 *      internal rep.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on object type.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclSetOtherValuePtr(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr, void *value)
{
    Ns_TclResetObjType(objPtr, newTypePtr);
    objPtr->internalRep.otherValuePtr = value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetStringRep --
 *
 *      Copy length bytes and set objects string rep.  The objects
 *      existing string rep *must* have already been freed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclSetStringRep(Tcl_Obj *objPtr, char *bytes, int length)
{
    if (length < 1) {
        length = strlen(bytes);
    }
    objPtr->length = length;
    objPtr->bytes = ckalloc((size_t) length + 1);
    memcpy(objPtr->bytes, bytes, (size_t) length + 1);
}

