/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

/*
 *----------------------------------------------------------------------
 *
 * nsatoms.c --
 *
 *      Implementation of the NaviServer global atom subsystem.
 *
 *      This module provides a small, shared registry of canonical
 *      string atoms backed by Tcl_Obj instances. A fixed set of
 *      core atoms with stable ids is initialized at startup, and
 *      additional atoms may be registered during module
 *      initialization before the registry is sealed.
 *
 *      The atom table is implemented as a dynamically resizable
 *      array indexed by NsAtomId. Each entry stores the canonical
 *      string, its length, and a shared Tcl_Obj representation.
 *
 *----------------------------------------------------------------------
 */

#include "ns.h"
#include "nsatoms.h"

typedef struct GlobalAtom {
    const char *name;      /* points to literal for core atoms, heap for dyn */
    TCL_SIZE_T  len;
    Tcl_Obj    *obj;
    bool        ownedName;
} GlobalAtom;

static GlobalAtom *atoms    = NULL;
static NsAtomId    nAtoms   = 0;    /* current count */
static NsAtomId    capAtoms = 0;    /* allocated capacity */

static Ns_Mutex atomLock = NULL;
static bool     atomInited = NS_FALSE;
static bool     atomSealed = NS_FALSE;

/*
 *----------------------------------------------------------------------
 *
 * EnsureCapacity --
 *
 *      Ensure that the global atom table has room for at least the requested
 *      number of entries. When the currently allocated capacity is too
 *      small, this function grows the backing storage (typically by doubling
 *      the capacity) and zero-initializes the newly added slots so callers
 *      can safely fill in new atom entries.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May reallocate the global atom table and update internal capacity
 *      tracking. Pointers into the old table become invalid after a growth.
 *      Newly allocated table elements are cleared to zero.
 *
 *----------------------------------------------------------------------
 */
static void
EnsureCapacity(NsAtomId need)
{
    if (need > capAtoms) {
        NsAtomId newCap = (capAtoms == 0) ? 32 : capAtoms;
        while (newCap < need) {
            newCap *= 2;
        }

        atoms = (GlobalAtom *)ns_realloc(atoms, (size_t)newCap * sizeof(GlobalAtom));
        memset(atoms + capAtoms, 0, (size_t)(newCap - capAtoms) * sizeof(GlobalAtom));
        capAtoms = newCap;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * InitCoreAtomSpecs --
 *
 *      Initialize the specification data for the core (built-in) atoms.
 *      For each core atom id, this function sets the atom's canonical
 *      string and its precomputed string length. The strings assigned
 *      here are core-owned literals (i.e., not heap allocated), so the
 *      corresponding entries are marked as not owning their name storage.
 *
 *      This function only populates the metadata (name/len/ownedName).
 *      Creation of the Tcl_Obj representations (and refcount management)
 *      is performed by the caller (typically NsAtomInit()).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Fills the global atom table entries for ids in the core range and
 *      sets ownedName to NS_FALSE for these atoms.
 *
 *----------------------------------------------------------------------
 */
static void
InitCoreAtomSpecs(void)
{
    atoms[NS_ATOM_EMPTY].name   = "";       atoms[NS_ATOM_EMPTY].len   = 0;
    atoms[NS_ATOM_TRUE].name    = "true";   atoms[NS_ATOM_TRUE].len    = 4;
    atoms[NS_ATOM_FALSE].name   = "false";  atoms[NS_ATOM_FALSE].len   = 5;
    atoms[NS_ATOM_ZERO].name    = "0";      atoms[NS_ATOM_ZERO].len    = 1;
    atoms[NS_ATOM_ONE].name     = "1";      atoms[NS_ATOM_ONE].len     = 1;

    atoms[NS_ATOM_ADDRESS].name          = "address";       atoms[NS_ATOM_ADDRESS].len = 7;
    atoms[NS_ATOM_ALLOCATED_DYNAMIC].name= "allocated_dynamic"; atoms[NS_ATOM_ALLOCATED_DYNAMIC].len = 17;
    atoms[NS_ATOM_ALLOCATED_STATIC].name = "allocated_static"; atoms[NS_ATOM_ALLOCATED_STATIC].len = 16;
    atoms[NS_ATOM_ALPN].name             = "alpn";          atoms[NS_ATOM_ALPN].len = 4;
    atoms[NS_ATOM_ASSERTIONS].name       = "assertions";    atoms[NS_ATOM_ASSERTIONS].len = 10;
    atoms[NS_ATOM_AUTHORITY].name        = "authority";     atoms[NS_ATOM_AUTHORITY].len = 9;
    atoms[NS_ATOM_BODY].name             = "body";          atoms[NS_ATOM_BODY].len = 4;
    atoms[NS_ATOM_BODY_CHAN].name        = "body_chan";     atoms[NS_ATOM_BODY_CHAN].len = 9;
    atoms[NS_ATOM_BROTLI].name           = "brotli";        atoms[NS_ATOM_BROTLI].len = 6;
    atoms[NS_ATOM_BYTES].name            = "bytes";         atoms[NS_ATOM_BYTES].len = 5;
    atoms[NS_ATOM_CALLBACK].name         = "callback";      atoms[NS_ATOM_CALLBACK].len = 8;
    atoms[NS_ATOM_CHANNEL].name          = "channel";       atoms[NS_ATOM_CHANNEL].len = 7;
    atoms[NS_ATOM_CIPHER].name           = "cipher";        atoms[NS_ATOM_CIPHER].len = 6;
    atoms[NS_ATOM_CODE].name             = "code";          atoms[NS_ATOM_CODE].len = 4;
    atoms[NS_ATOM_COMPILER].name         = "compiler";      atoms[NS_ATOM_COMPILER].len = 8;
    atoms[NS_ATOM_COMPLETE].name         = "complete";      atoms[NS_ATOM_COMPLETE].len = 8;
    atoms[NS_ATOM_CONDITION].name        = "condition";     atoms[NS_ATOM_CONDITION].len = 9;
    atoms[NS_ATOM_CURRENTADDR].name      = "currentaddr";   atoms[NS_ATOM_CURRENTADDR].len = 11;
    atoms[NS_ATOM_DATA].name             = "data";          atoms[NS_ATOM_DATA].len = 4;
    atoms[NS_ATOM_DRIVER].name           = "driver";        atoms[NS_ATOM_DRIVER].len = 6;
    atoms[NS_ATOM_ERROR].name            = "error";         atoms[NS_ATOM_ERROR].len = 5;
    atoms[NS_ATOM_EXCEPTION].name        = "exception";     atoms[NS_ATOM_EXCEPTION].len = 9;
    atoms[NS_ATOM_EXPIRE].name           = "expire";        atoms[NS_ATOM_EXPIRE].len = 6;
    atoms[NS_ATOM_FILE].name             = "file";          atoms[NS_ATOM_FILE].len = 4;
    atoms[NS_ATOM_FIN].name              = "fin";           atoms[NS_ATOM_FIN].len = 3;
    atoms[NS_ATOM_FIRSTLINE].name        = "firstline";     atoms[NS_ATOM_FIRSTLINE].len = 9;
    atoms[NS_ATOM_FLAGS].name            = "flags";         atoms[NS_ATOM_FLAGS].len = 5;
    atoms[NS_ATOM_FRAGMENTS].name        = "fragments";     atoms[NS_ATOM_FRAGMENTS].len = 9;
    atoms[NS_ATOM_FRAGMENT].name         = "fragment";      atoms[NS_ATOM_FRAGMENT].len = 8;
    atoms[NS_ATOM_FRAMEBUFFER].name      = "framebuffer";   atoms[NS_ATOM_FRAMEBUFFER].len = 11;
    atoms[NS_ATOM_FRAME].name            = "frame";         atoms[NS_ATOM_FRAME].len = 5;
    atoms[NS_ATOM_GZIP].name             = "gzip";          atoms[NS_ATOM_GZIP].len = 4;
    atoms[NS_ATOM_HANDLER].name          = "handler";       atoms[NS_ATOM_HANDLER].len = 7;
    atoms[NS_ATOM_HAVEDATA].name         = "havedata";      atoms[NS_ATOM_HAVEDATA].len = 8;
    atoms[NS_ATOM_HEADERS].name          = "headers";       atoms[NS_ATOM_HEADERS].len = 7;
    atoms[NS_ATOM_HOST].name             = "host";          atoms[NS_ATOM_HOST].len = 4;
    atoms[NS_ATOM_HOST].name             = "host";          atoms[NS_ATOM_HOST].len = 4;
    atoms[NS_ATOM_HTTPS].name            = "https";         atoms[NS_ATOM_HTTPS].len = 5;
    atoms[NS_ATOM_HTTPVERSION].name      = "httpversion";   atoms[NS_ATOM_HTTPVERSION].len = 11;
    atoms[NS_ATOM_INANY].name            = "inany";         atoms[NS_ATOM_INANY].len = 5;
    atoms[NS_ATOM_INCOMPLETE].name       = "incomplete";    atoms[NS_ATOM_INCOMPLETE].len = 10;
    atoms[NS_ATOM_NAME].name             = "name";          atoms[NS_ATOM_NAME].len = 4;
    atoms[NS_ATOM_NR_DYNAMIC].name       = "nr_dynamic";    atoms[NS_ATOM_NR_DYNAMIC].len = 10;
    atoms[NS_ATOM_NR_STATIC].name        = "nr_static";     atoms[NS_ATOM_NR_STATIC].len = 9;
    atoms[NS_ATOM_OPCODE].name           = "opcode";        atoms[NS_ATOM_OPCODE].len = 6;
    atoms[NS_ATOM_OUTPUTCHAN].name       = "outputchan";    atoms[NS_ATOM_OUTPUTCHAN].len = 10;
    atoms[NS_ATOM_PATH].name             = "path";          atoms[NS_ATOM_PATH].len = 4;
    atoms[NS_ATOM_PAYLOAD].name          = "payload";       atoms[NS_ATOM_PAYLOAD].len = 7;
    atoms[NS_ATOM_PEER].name             = "peer";          atoms[NS_ATOM_PEER].len = 4;
    atoms[NS_ATOM_PHRASE].name           = "phrase";        atoms[NS_ATOM_PHRASE].len = 6;
    atoms[NS_ATOM_POOL].name             = "pool";          atoms[NS_ATOM_POOL].len = 4;
    atoms[NS_ATOM_PORT].name             = "port";          atoms[NS_ATOM_PORT].len = 4;
    atoms[NS_ATOM_PORT].name             = "port";          atoms[NS_ATOM_PORT].len = 4;
    atoms[NS_ATOM_PRELOAD].name          = "preload";       atoms[NS_ATOM_PRELOAD].len = 7;
    atoms[NS_ATOM_PROTO].name            = "proto";         atoms[NS_ATOM_PROTO].len = 5;
    atoms[NS_ATOM_PROXIED].name          = "proxied";       atoms[NS_ATOM_PROXIED].len = 7;
    atoms[NS_ATOM_PUBLIC].name           = "public";        atoms[NS_ATOM_PUBLIC].len = 6;
    atoms[NS_ATOM_QUERY].name            = "query";         atoms[NS_ATOM_QUERY].len = 5;
    atoms[NS_ATOM_RECEIVED].name         = "received";      atoms[NS_ATOM_RECEIVED].len = 8;
    atoms[NS_ATOM_RECVERROR].name        = "recverror";     atoms[NS_ATOM_RECVERROR].len = 9;
    atoms[NS_ATOM_REPLYBODYSIZE].name    = "replybodysize"; atoms[NS_ATOM_REPLYBODYSIZE].len = 13;
    atoms[NS_ATOM_REPLYLENGTH].name      = "replylength";   atoms[NS_ATOM_REPLYLENGTH].len = 11;
    atoms[NS_ATOM_REPLYSIZE].name        = "replysize";     atoms[NS_ATOM_REPLYSIZE].len = 9;
    atoms[NS_ATOM_REQUESTLENGTH].name    = "requestlength"; atoms[NS_ATOM_REQUESTLENGTH].len = 13;
    atoms[NS_ATOM_REQUESTS].name         = "requests";      atoms[NS_ATOM_REQUESTS].len = 8;
    atoms[NS_ATOM_RUNNING].name          = "running";       atoms[NS_ATOM_RUNNING].len = 7;
    atoms[NS_ATOM_SENDBODYSIZE].name     = "sendbodysize";  atoms[NS_ATOM_SENDBODYSIZE].len = 12;
    atoms[NS_ATOM_SENDBUFFER].name       = "sendbuffer";    atoms[NS_ATOM_SENDBUFFER].len = 10;
    atoms[NS_ATOM_SENDERROR].name        = "senderror";     atoms[NS_ATOM_SENDERROR].len = 9;
    atoms[NS_ATOM_SENT].name             = "sent";          atoms[NS_ATOM_SENT].len = 4;
    atoms[NS_ATOM_SERVERNAME].name       = "servername";    atoms[NS_ATOM_SERVERNAME].len = 10;
    atoms[NS_ATOM_SIZE_DYNAMIC].name     = "size_dynamic";  atoms[NS_ATOM_SIZE_DYNAMIC].len = 12;
    atoms[NS_ATOM_SIZE_STATIC].name      = "size_static";   atoms[NS_ATOM_SIZE_STATIC].len = 11;
    atoms[NS_ATOM_SLOT].name             = "slot";          atoms[NS_ATOM_SLOT].len = 4;
    atoms[NS_ATOM_SOCK].name             = "sock";          atoms[NS_ATOM_SOCK].len = 4;
    atoms[NS_ATOM_SSLVERSION].name       = "sslversion";    atoms[NS_ATOM_SSLVERSION].len = 10;
    atoms[NS_ATOM_START].name            = "start";         atoms[NS_ATOM_START].len = 5;
    atoms[NS_ATOM_STATE].name            = "state";         atoms[NS_ATOM_STATE].len = 5;
    atoms[NS_ATOM_STATS].name            = "stats";         atoms[NS_ATOM_STATS].len = 5;
    atoms[NS_ATOM_STATUS].name           = "status";        atoms[NS_ATOM_STATUS].len = 6;
    atoms[NS_ATOM_SYSTEM_MALLOC].name    = "system_malloc"; atoms[NS_ATOM_SYSTEM_MALLOC].len = 13;
    atoms[NS_ATOM_TAIL].name             = "tail";          atoms[NS_ATOM_TAIL].len = 4;
    atoms[NS_ATOM_TASK].name             = "task";          atoms[NS_ATOM_TASK].len = 4;
    atoms[NS_ATOM_TCL].name              = "tcl";           atoms[NS_ATOM_TCL].len = 3;
    atoms[NS_ATOM_TIME].name             = "time";          atoms[NS_ATOM_TIME].len = 4;
    atoms[NS_ATOM_TRUSTED].name          = "trusted";       atoms[NS_ATOM_TRUSTED].len = 7;
    atoms[NS_ATOM_TUNNEL].name           = "tunnel";        atoms[NS_ATOM_TUNNEL].len = 6;
    atoms[NS_ATOM_TYPE].name             = "type";          atoms[NS_ATOM_TYPE].len = 4;
    atoms[NS_ATOM_UNPROCESSED].name      = "unprocessed";   atoms[NS_ATOM_UNPROCESSED].len = 11;
    atoms[NS_ATOM_URL].name              = "url";           atoms[NS_ATOM_URL].len = 3;
    atoms[NS_ATOM_USERINFO].name         = "userinfo";      atoms[NS_ATOM_USERINFO].len = 8;
    atoms[NS_ATOM_VERSION].name          = "version";       atoms[NS_ATOM_VERSION].len = 7;
    atoms[NS_ATOM_WITH_DEPRECATED].name  = "with_deprecated"; atoms[NS_ATOM_WITH_DEPRECATED].len = 15;

    for (NsAtomId i = 0; i < (NsAtomId)NS_ATOM__CORE_MAX; i++) {
        atoms[i].ownedName = NS_FALSE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomCoreInit --
 *
 *      Initialize the global atom table with the built-in (core) atoms.
 *      The core atom ids are stable and correspond to the constants in
 *      the NsCoreAtomId enumeration. This function allocates the initial
 *      atom table (if needed), populates the core atom specifications
 *      (name and precomputed length), and creates the corresponding
 *      Tcl_Obj string objects with incremented reference counts.
 *
 *      After this function returns, the atom table is ready to serve
 *      NsAtomObj() and NsAtomName() requests for core atoms, and the
 *      registry is open for optional module registrations until it is
 *      closed via NsAtomSeal().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates and initializes global atom table storage, creates
 *      core Tcl_Obj instances and increments their refcounts, and
 *      resets internal initialization/seal state.
 *
 *----------------------------------------------------------------------
 */
void
NsAtomCoreInit(void)
{
    Ns_MutexInit(&atomLock);
    Ns_MutexSetName(&atomLock, "nsd:atoms");

    Ns_MutexLock(&atomLock);

    if (atomInited) {
        Ns_MutexUnlock(&atomLock);
        return;
    }
    atomInited = NS_TRUE;
    atomSealed = NS_FALSE;

    EnsureCapacity((NsAtomId)NS_ATOM__CORE_MAX);
    nAtoms = (NsAtomId)NS_ATOM__CORE_MAX;

    InitCoreAtomSpecs();

    for (NsAtomId i = 0; i < nAtoms; i++) {
        Tcl_Obj *o = Tcl_NewStringObj(atoms[i].name, atoms[i].len);
        Tcl_IncrRefCount(o);
        atoms[i].obj = o;
    }

    Ns_MutexUnlock(&atomLock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomSeal --
 *
 *      Close the global atom registry and reject further dynamic atom
 *      registrations. This is intended to be called once startup is
 *      complete (e.g., after all modules have performed their optional
 *      NsAtomRegister() calls). After sealing, only read access via
 *      NsAtomObj() and NsAtomName() remains valid.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets the internal "sealed" flag. Subsequent calls to
 *      NsAtomRegister() will fail.
 *
 *----------------------------------------------------------------------
 */
void
NsAtomSeal(void)
{
    Ns_MutexLock(&atomLock);
    atomSealed = NS_TRUE;
    Ns_MutexUnlock(&atomLock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomSealed --
 *
 *      Report whether the global atom registry has been sealed. When
 *      sealed, no further dynamic atom registrations are permitted.
 *
 * Results:
 *      NS_TRUE when the registry is sealed, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
NsAtomSealed(void)
{
    return atomSealed;
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomMax --
 *
 *      Return the current number of global atoms in the atom table,
 *      including the built-in core atoms and any atoms registered during
 *      startup. The valid id range for NsAtomObj() and NsAtomName() is
 *      [0 .. NsAtomMax()-1].
 *
 * Results:
 *      The current size of the global atom table.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
NsAtomId
NsAtomMax(void)
{
    return nAtoms;
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomShutdown --
 *
 *      Tear down the global atom table and release all resources owned
 *      by the atom subsystem. This function decrements the reference
 *      counts of all Tcl_Obj instances created for global atoms, frees
 *      any dynamically allocated atom names, and releases the atom table
 *      storage. After shutdown, the atom subsystem returns to the
 *      uninitialized state and must be reinitialized via NsAtomCoreInit()
 *      (or NsAtomInit(), depending on the surrounding startup logic)
 *      before further use.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Decrements Tcl_Obj refcounts, frees dynamically allocated atom
 *      names, frees the global atom table, and resets internal state
 *      flags and counters.
 *
 *----------------------------------------------------------------------
 */
void
NsAtomShutdown(void)
{
    Ns_MutexLock(&atomLock);

    if (!atomInited) {
        Ns_MutexUnlock(&atomLock);
        return;
    }
    atomInited = NS_FALSE;
    atomSealed = NS_FALSE;

    for (NsAtomId i = 0; i < nAtoms; i++) {
        if (atoms[i].obj != NULL) {
            Tcl_DecrRefCount(atoms[i].obj);
            atoms[i].obj = NULL;
        }
        if (atoms[i].ownedName && atoms[i].name != NULL) {
            ns_free((char *)atoms[i].name);
            atoms[i].name = NULL;
            atoms[i].ownedName = NS_FALSE;
        }
        atoms[i].len = 0;
    }

    ns_free(atoms);
    atoms = NULL;
    nAtoms = 0;
    capAtoms = 0;

    Ns_MutexUnlock(&atomLock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomObj --
 *
 *      Return the Tcl_Obj representation associated with the specified
 *      global atom id. The returned object is the shared, precreated
 *      string object stored in the global atom table. Callers must not
 *      modify or decrement the reference count of the returned object.
 *
 * Results:
 *      The Tcl_Obj corresponding to the atom id, or NULL if the id is
 *      out of range.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Tcl_Obj *
NsAtomObj(NsAtomId id)
{
    assert((unsigned)id < (unsigned)nAtoms);
    return atoms[id].obj;
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomName --
 *
 *      Return the canonical string name associated with the specified
 *      global atom id. Optionally returns the precomputed string length
 *      via lenPtr when non-NULL.
 *
 *      The returned pointer refers to storage owned by the atom table
 *      (either a static literal for core atoms or heap-allocated storage
 *      for dynamically registered atoms) and must not be modified or
 *      freed by the caller.
 *
 * Results:
 *      The atom name as a NUL-terminated string, or NULL if the id is
 *      out of range. When lenPtr is non-NULL and the id is valid, *lenPtr
 *      receives the string length.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
NsAtomName(NsAtomId id, TCL_SIZE_T *lenPtr)
{
    if ((unsigned)id < (unsigned)nAtoms) {
        return NULL;
    }
    if (lenPtr != NULL) {
        *lenPtr = atoms[id].len;
    }
    return atoms[id].name;
}


/*
 *----------------------------------------------------------------------
 *
 * FindAtomByName --
 *
 *      Perform a linear search over the global atom table to locate an
 *      atom with the specified name and length. Comparison is performed
 *      using the precomputed length and a byte-wise memory comparison.
 *
 *      This function is intended for use during startup when the total
 *      number of atoms is small. It does not use hashing and therefore
 *      runs in O(nAtoms) time.
 *
 * Results:
 *      The atom id of the matching entry, or -1 when no atom with the
 *      specified name exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static NsAtomId
FindAtomByName(const char *name, TCL_SIZE_T len)
{
    for (NsAtomId i = 0; i < nAtoms; i++) {
        if (atoms[i].len == len && memcmp(atoms[i].name, name, (size_t)len) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomRegister --
 *
 *      Register a new global atom during startup. If an atom with the
 *      specified name already exists, its existing id is returned. If
 *      not, a new atom entry is appended to the global atom table and
 *      assigned a new id.
 *
 *      Registration is only permitted after initialization and before
 *      the registry has been sealed via NsAtomSeal(). Attempts to
 *      register atoms after sealing fail.
 *
 *      The name is copied into owned storage for dynamically registered
 *      atoms. A corresponding Tcl string object is created and its
 *      reference count incremented.
 *
 * Results:
 *      NS_OK on success (whether newly created or already existing),
 *      NS_ERROR when registration is not permitted or initialization
 *      has not occurred.
 *
 * Side effects:
 *      May grow the global atom table, allocate memory for the atom
 *      name, create a new Tcl_Obj and increment its reference count,
 *      and update internal bookkeeping.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsAtomRegister(const char *name, TCL_SIZE_T len, NsAtomId *idPtr)
{
    Ns_ReturnCode rc = NS_OK;

    Ns_MutexLock(&atomLock);

    if (!atomInited || atomSealed) {
        rc = NS_ERROR;
        goto done;
    }

    if (len < 0) {
        len = (TCL_SIZE_T)strlen(name);
    }

    {
        NsAtomId existing = FindAtomByName(name, len);
        if (existing >= 0) {
            *idPtr = existing;
            goto done;
        }
    }

    EnsureCapacity(nAtoms + 1);

    /*
     * Store a NUL-terminated owned copy for dynamically registered atoms.
     */
    {
        char *copy = (char *)ns_malloc((size_t)len + 1u);
        memcpy(copy, name, (size_t)len);
        copy[len] = '\0';

        atoms[nAtoms].name = copy;
        atoms[nAtoms].len  = len;
        atoms[nAtoms].ownedName = NS_TRUE;

        atoms[nAtoms].obj = Tcl_NewStringObj(atoms[nAtoms].name, atoms[nAtoms].len);
        Tcl_IncrRefCount(atoms[nAtoms].obj);

        *idPtr = nAtoms;
        nAtoms++;
    }

 done:
    Ns_MutexUnlock(&atomLock);
    return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAtomsInit --
 *
 *      Initialize a module-local vector of Tcl_Obj pointers from an
 *      array of NsAtomSpec entries. For each spec entry, either a
 *      reference to an existing global atom object is stored (when
 *      globalId >= 0), or a new Tcl string object is created from the
 *      provided literal (when globalId < 0).
 *
 *      For module-owned atoms (globalId < 0), the created Tcl_Obj has
 *      its reference count incremented and must later be released via
 *      NsAtomsFreeOwned() or equivalent manual DecrRefCount handling.
 *
 * Results:
 *      NS_OK on success, NS_ERROR if a referenced global atom id is
 *      invalid.
 *
 * Side effects:
 *      May create new Tcl string objects and increment their reference
 *      counts for module-owned atoms.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsAtomsInit(const NsAtomSpec *specs, size_t nSpecs, Tcl_Obj **outAtoms)
{
    for (size_t i = 0; i < nSpecs; i++) {
        if (specs[i].globalId >= 0) {
            outAtoms[i] = NsAtomObj(specs[i].globalId);
            if (outAtoms[i] == NULL) {
                return NS_ERROR;
            }
        } else {
            TCL_SIZE_T len = specs[i].len;

            if (len < 0) {
                len = (TCL_SIZE_T)strlen(specs[i].name);
            }
            outAtoms[i] = Tcl_NewStringObj(specs[i].name, len);
            Tcl_IncrRefCount(outAtoms[i]); /* module-owned */
        }
    }
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsAtomsFreeOwned --
 *
 *      Decrement the reference counts of module-owned Tcl_Obj instances
 *      previously created by NsAtomsInit(). Only entries whose spec has
 *      globalId < 0 are decremented; references to shared global atoms
 *      are left untouched.
 *
 *      This function is intended to be called during module shutdown
 *      or cleanup to properly release module-owned atom objects.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Decrements reference counts of module-owned Tcl_Obj instances
 *      and clears the corresponding output vector entries.
 *
 *----------------------------------------------------------------------
 */
void
NsAtomsFreeOwned(const NsAtomSpec *specs, size_t nSpecs, Tcl_Obj **atomsVec)
{
    for (size_t i = 0; i < nSpecs; i++) {
        if (specs[i].globalId < 0 && atomsVec[i] != NULL) {
            Tcl_DecrRefCount(atomsVec[i]);
            atomsVec[i] = NULL;
        }
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
