/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/ */
#ifndef NSDB_BATCH_H
# define NSDB_BATCH_H
# include "ns.h"

# define NS_DB_BATCH_ABI 1u
# define NS_DB_BATCH_ASSOC_DATA "nsdb:batch-api"

typedef struct Ns_DbBatch       Ns_DbBatch;
typedef struct Ns_DbBatchSchema Ns_DbBatchSchema;
struct Ns_DbHandle;

/*
 * NULL refinement/representation means the JSON base type without refinement.
 */
typedef struct {
    const char *name, *type, *refinement, *representation;
    bool        nullable;
} Ns_DbBatchColumn;

typedef struct {
    const unsigned char *bytes;
    size_t               length;
    bool                 isNull;
} Ns_DbBatchValue;

/*
 * Borrowed views: valid while the immutable producer payload remains
 * retained.  C readers and Ns_DbBatchGetValue return NS_OK/NS_ERROR,
 * not Tcl statuses.
 */
typedef Ns_ReturnCode(Ns_DbBatchReadProc)(const void *, size_t, size_t, Ns_DbBatchValue *)
    NS_GNUC_NONNULL(4);
typedef void(Ns_DbBatchFreeProc)(void *);

typedef struct {
    unsigned int abiVersion;
    size_t       structSize;
    Ns_DbBatchSchema *(*schemaCreate)(Tcl_Interp *, Tcl_Obj *)NS_GNUC_NONNULL(1, 2);
    void (*schemaRelease)(Ns_DbBatchSchema *) NS_GNUC_NONNULL(1);
    /* Adopt payload on success only. No row copy or Tcl value materialization. */
    Tcl_Obj *(*adopt)(Tcl_Interp *, Ns_DbBatchSchema *, Tcl_Obj *, size_t, void *,
                      Ns_DbBatchReadProc *, Ns_DbBatchFreeProc *)NS_GNUC_NONNULL(1, 2, 3, 6, 7);
    int (*get)(Tcl_Interp *, Tcl_Obj *, const Ns_DbBatch **) NS_GNUC_NONNULL(1, 2, 3);
    Tcl_Obj *(*info)(const Ns_DbBatch *)NS_GNUC_NONNULL(1);
    int (*triples)(Tcl_Interp *, const Ns_DbBatch *, size_t, size_t) NS_GNUC_NONNULL(1, 2);
} Ns_DbBatchApi;

typedef enum {
    NS_DB_BATCH_IDLE,
    NS_DB_BATCH_ACTIVE,
    NS_DB_BATCH_FAILED,
    NS_DB_BATCH_UNKNOWN
} Ns_DbBatchTransaction;

typedef struct {
    size_t      source;
    const char *destination;
} Ns_DbBatchMapping;

typedef struct {
    unsigned int abiVersion;
    size_t       structSize;
    const char  *method;
    bool (*supports)(const Ns_DbBatchColumn *) NS_GNUC_NONNULL(1);
    Tcl_Obj *(*capabilities)(void);
    Ns_DbBatchTransaction (*transaction)(struct Ns_DbHandle *) NS_GNUC_NONNULL(1);
    int (*insert)(Tcl_Interp *, struct Ns_DbHandle *, const Ns_DbBatch *, Tcl_Obj *,
                  const Ns_DbBatchMapping *, size_t, Tcl_WideInt *)
        NS_GNUC_NONNULL(1, 2, 3, 4, 5, 7);
} Ns_DbBatchDriverOps;

/*
 * DbFn_Batch registers this accessor in the ordinary Ns_DbProc table.
 * Return an immutable operations table valid for the driver's
 * lifetime.
 */
typedef const Ns_DbBatchDriverOps *(Ns_DbBatchDriverProc)(void);
NS_EXTERN void                    Ns_DbBatchRetain(Ns_DbBatch *) NS_GNUC_NONNULL(1);
NS_EXTERN void                    Ns_DbBatchRelease(Ns_DbBatch *) NS_GNUC_NONNULL(1);
NS_EXTERN size_t                  Ns_DbBatchRowCount(const Ns_DbBatch *) NS_GNUC_NONNULL(1);
NS_EXTERN size_t                  Ns_DbBatchColumnCount(const Ns_DbBatch *) NS_GNUC_NONNULL(1);
NS_EXTERN const Ns_DbBatchColumn *Ns_DbBatchGetColumn(const Ns_DbBatch *, size_t)
    NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbBatchGetValue(const Ns_DbBatch *, size_t, size_t, Ns_DbBatchValue *)
    NS_GNUC_NONNULL(1, 4);
NS_EXTERN int Ns_DbBatchError(Tcl_Interp *, const char *, const char *) NS_GNUC_NONNULL(1, 2, 3);
#endif
