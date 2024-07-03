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
 * tclobj.c --
 *
 *      Helper routines for managing Tcl_Obj types.
 */

#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static Tcl_UpdateStringProc UpdateStringOfAddr;
static Tcl_SetFromAnyProc   SetAddrFromAny;

/*
 * Local variables defined in this file.
 */

static CONST86 Tcl_ObjType addrType = {
    "ns:addr",
    NULL,
    NULL,
    UpdateStringOfAddr,
    SetAddrFromAny
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
};

static CONST86 Tcl_ObjType *byteArrayTypePtr; /* For NsTclObjIsByteArray(). */
static CONST86 Tcl_ObjType *properByteArrayTypePtr;  /* For NsTclObjIsByteArray(). */

/*
 *----------------------------------------------------------------------
 *
 * NsTclInitAddrType --
 *
 *      Initialize the Tcl address object type and cache the bytearray Tcl
 *      built-in type. Starting with Tcl 8.7a1, Tcl has actually two different
 *      types for bytearrays, the old "tclByteArrayType" and a new
 *      "properByteArrayType", where both have the string name "bytearray".
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
NsTclInitAddrType(void)
{
    Tcl_Obj *newByteObj;

    Tcl_RegisterObjType(&addrType);
    /*
     * Get the "tclByteArrayType" via name "bytearray".
     */
    byteArrayTypePtr = Tcl_GetObjType("bytearray");

    /*
     * Get the "properByteArrayType" via a TclObj.
     * In versions before Tcl 8.7, both values will be the same.
     */
    newByteObj = Tcl_NewByteArrayObj(NULL, 0);
    properByteArrayTypePtr = newByteObj->typePtr;
    if (properByteArrayTypePtr == byteArrayTypePtr) {
        /*
         * When both values are the same, we are in a Tcl version before 8.7,
         * where we have no properByteArrayTypePtr. So set it to an invalid
         * value to avoid potential confusions. Without this stunt, we would
         * need several ifdefs.
         */
        properByteArrayTypePtr = (Tcl_ObjType *)INT2PTR(0xffffff);
    }
    Tcl_DecrRefCount(newByteObj);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclResetObjType --
 *
 *      Reset the given Tcl_Obj type, freeing any type specific
 *      internal representation. The new Tcl_Obj type might be NULL.
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
Ns_TclResetObjType(Tcl_Obj *objPtr, CONST86 Tcl_ObjType *newTypePtr)
{
    const Tcl_ObjType *typePtr;

    NS_NONNULL_ASSERT(objPtr != NULL);

    typePtr = objPtr->typePtr;
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
Ns_TclSetTwoPtrValue(Tcl_Obj *objPtr, CONST86 Tcl_ObjType *newTypePtr,
                     void *ptr1, void *ptr2)
{
    NS_NONNULL_ASSERT(objPtr != NULL);

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
Ns_TclSetOtherValuePtr(Tcl_Obj *objPtr, CONST86 Tcl_ObjType *newTypePtr, void *value)
{
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(newTypePtr != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    Ns_TclResetObjType(objPtr, newTypePtr);
    objPtr->internalRep.otherValuePtr = value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetStringRep --
 *
 *      Copy length bytes and set objects string rep.  The objects
 *      existing string rep *must* have already been freed. Tcl uses
 *      as well "int" and not "size" (internally and via interface)
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
Ns_TclSetStringRep(Tcl_Obj *objPtr, const char *bytes, TCL_SIZE_T length)
{
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(bytes != NULL);

    if (length == TCL_INDEX_NONE) {
        length = (TCL_SIZE_T)strlen(bytes);
    }
    objPtr->length = length;
    objPtr->bytes = ckalloc((unsigned) length + 1u);
    memcpy(objPtr->bytes, bytes, (size_t) length + 1u);
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetFromAnyError --
 *
 *      This procedure is registered as the setFromAnyProc for an
 *      object type when it doesn't make sense to generate its internal
 *      form from the string representation alone.
 *
 * Results:
 *      The return value is always TCL_ERROR, and an error message is
 *      left in interp's result if interp isn't NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclSetFromAnyError(Tcl_Interp *interp, Tcl_Obj *UNUSED(objPtr))
{
    Tcl_AppendToObj(Tcl_GetObjResult(interp),
                    "can't convert value to requested type except via prescribed API",
                    TCL_INDEX_NONE);
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetAddrFromObj --
 *
 *      Return the internal pointer of an address Tcl_Obj.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if conversion failed or not the correct type.
 *
 * Side effects:
 *      Object may be converted to address type.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclGetAddrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                     const char *type, void **addrPtrPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);
    NS_NONNULL_ASSERT(addrPtrPtr != NULL);

    if (Tcl_ConvertToType(interp, objPtr, &addrType) != TCL_OK) {
        result = TCL_ERROR;

    } else if (objPtr->internalRep.twoPtrValue.ptr1 != (void *) type) {
        Ns_TclPrintfResult(interp, "incorrect type: %s", Tcl_GetString(objPtr));
        result = TCL_ERROR;

    } else {
        *addrPtrPtr = objPtr->internalRep.twoPtrValue.ptr2;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetAddrObj --
 *
 *      Convert the given object to the ns:addr type.
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
Ns_TclSetAddrObj(Tcl_Obj *objPtr, const char *type, void *addr)
{
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);
    NS_NONNULL_ASSERT(addr != NULL);

    if (Tcl_IsShared(objPtr)) {
        Tcl_Panic("Ns_TclSetAddrObj called with shared object");
    }
    Ns_TclSetTwoPtrValue(objPtr, &addrType, (void *) type, addr);
    Tcl_InvalidateStringRep(objPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetOpaqueFromObj --
 *
 *      Get the internal pointer of an address Tcl_Obj.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if object was not of the ns:addr type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclGetOpaqueFromObj(const Tcl_Obj *objPtr, const char *type, void **addrPtrPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);
    NS_NONNULL_ASSERT(addrPtrPtr != NULL);

    if (objPtr->typePtr == &addrType
        && objPtr->internalRep.twoPtrValue.ptr1 == (void *) type) {
        *addrPtrPtr = objPtr->internalRep.twoPtrValue.ptr2;
    } else {
        char      s[33] = {0};
        uintptr_t t = 0u, a = 0u;

        if ((sscanf(Tcl_GetString((Tcl_Obj *) objPtr), "t%20" SCNxPTR "-a%20" SCNxPTR "-%32s", &t, &a, s) != 3)
            || (strcmp(s, type) != 0)
            || (t != (uintptr_t)type)
            ) {
            result = TCL_ERROR;
        } else {
            *addrPtrPtr = (void *)a;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetOpaqueObj --
 *
 *      Convert the given object to the ns:addr type without
 *      invalidating the current string rep.  It is OK if the object
 *      is shared.
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
Ns_TclSetOpaqueObj(Tcl_Obj *objPtr, const char *type, void *addr)
{
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);

    Ns_TclSetTwoPtrValue(objPtr, &addrType, (void *) type, addr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclObjIsByteArray --
 *
 *      Does the given Tcl_Obj have a byte array internal rep?  The
 *      function determines when it is safe to interpret a string as a
 *      byte array directly.
 *
 * Results:
 *      Boolean.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
NsTclObjIsByteArray(const Tcl_Obj *objPtr)
{
    bool result;

    NS_NONNULL_ASSERT(objPtr != NULL);

    /*
     * This function resembles the tclInt.h function for testing pure byte
     * arrays. In versions up to at least on Tcl 8.6, a pure byte array was
     * defined as a byte array without a string rep.  Starting with Tcl
     * 8.7a1, Tcl has introduced the properByteArrayTypePtr, which allows as
     * well a string rep.
     */
    Ns_Log(Debug, "NsTclObjIsByteArray %p byteArrayTypePtr %d properByteArrayTypePtr %d"
           "objPtr->bytes %p",
           (void*)objPtr,
           (objPtr->typePtr == byteArrayTypePtr),
           (objPtr->typePtr == properByteArrayTypePtr),
           (void*)objPtr->bytes);
#ifdef NS_TCL_PRE87
    result = ((objPtr->typePtr == byteArrayTypePtr) && (objPtr->bytes == NULL));
#else
    result = (objPtr->typePtr == properByteArrayTypePtr) && (objPtr->bytes == NULL);
#endif

#if 0
    fprintf(stderr, "NsTclObjIsByteArray? %p type %p proper %d old %d bytes %p name %s => %d\n",
            (void*)objPtr,
            (void*)(objPtr->typePtr),
            (objPtr->typePtr == properByteArrayTypePtr),
            (objPtr->typePtr == byteArrayTypePtr),
            (void*)(objPtr->bytes),
            objPtr->typePtr == NULL ? "string" : objPtr->typePtr->name,
            result);
#endif

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclObjIsEncodedByteArray --
 *
 *      This function is true, when we encounter a bytearray with a string
 *      rep.  In this cases, it is necessary to use Tcl_UtfToExternalDString()
 *      to obtain the proper byte array.
 *
 * Results:
 *      Boolean.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
NsTclObjIsEncodedByteArray(const Tcl_Obj *objPtr)
{
    NS_NONNULL_ASSERT(objPtr != NULL);

    return ((objPtr->typePtr == byteArrayTypePtr) && (objPtr->bytes != NULL));
}


/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfAddr --
 *
 *      Update the string representation for an address object.
 *      Note: This procedure does not free an existing old string rep
 *      so storage will be lost if this has not already been done.
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
UpdateStringOfAddr(Tcl_Obj *objPtr)
{
    const char *type = objPtr->internalRep.twoPtrValue.ptr1;
    const void *addr = objPtr->internalRep.twoPtrValue.ptr2;
    char        buf[128];
    TCL_SIZE_T  len;

    len = (TCL_SIZE_T)snprintf(buf, sizeof(buf),
                               "t%" PRIxPTR "-a%" PRIxPTR "-%s",
                               (uintptr_t)type,
                               (uintptr_t)addr,
                               type);
    Ns_TclSetStringRep(objPtr, buf, len);
}


/*
 *----------------------------------------------------------------------
 *
 * SetAddrFromAny --
 *
 *      Attempt to generate an address internal form for the Tcl_Obj.
 *
 * Results:
 *      The return value is a standard Tcl result. If an error occurs
 *      during conversion, an error message is left in the interpreter's
 *      result unless interp is NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SetAddrFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    int   result = TCL_OK;
    void *type, *addr;
    char *chars;

    chars = Tcl_GetString(objPtr);
    if ((sscanf(chars, "t%20p-a%20p", &type, &addr) != 2)
        || (type == NULL)
        || (addr == NULL)
        ) {
        Ns_TclPrintfResult(interp, "invalid address \"%s\"", chars);
        result = TCL_ERROR;
    } else {
        Ns_TclSetTwoPtrValue(objPtr, &addrType, type, addr);
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
