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
 * random.c --
 *
 *      This file implements the "ns_rand" command.
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static Ns_ThreadProc CounterThread;
static unsigned long TrueRand(void);
static unsigned long Roulette(void);

/*
 * Static variables used by Ns_GenSeeds to generate array of random
 * by utilizing the random nature of the thread scheduler.
 */

static volatile unsigned long counter = 0u;  /* Counter in counting thread */
static volatile bool fRun = NS_FALSE;        /* Flag for counting thread outer loop. */
static volatile bool fCount = NS_FALSE;      /* Flag for counting thread inner loop. */
static Ns_Sema       sema = NULL;            /* Semaphore that controls counting threads. */

/*
 * Critical section around initial and subsequent seed generation.
 */

static Ns_Cs lock = NULL;
static volatile bool initialized = NS_FALSE;

/*
 * Static functions defined in this file.
 */

static void GenSeeds(unsigned long seeds[], int nseeds);


/*
 *----------------------------------------------------------------------
 *
 * NsTclRandObjCmd --
 *
 *      Implements "ns_rand".
 *
 * Results:
 *      The Tcl result string contains a random number, either a
 *      double >= 0.0 and < 1.0 or an integer >= 0 and < max.
 *
 * Side effects:
 *      None external.
 *
 * Note:
 *      Interpreters share the static variables which randomizes the
 *      the random numbers even more.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRandObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int               maxValue = -1, result = TCL_OK;
    Ns_ObjvValueRange range = {1, INT_MAX};
    Ns_ObjvSpec       args[] = {
        {"?maximum", Ns_ObjvInt, &maxValue, &range},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        double d = Ns_DRand();

        if (maxValue != -1) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj((int) (d * (double)maxValue)));
        }  else {
            Tcl_SetObjResult(interp, Tcl_NewDoubleObj(d));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DRand --
 *
 *      Return a random double value between 0 and 1.0.
 *
 * Results:
 *      Random double.
 *
 * Side effects:
 *      Will generate random seed on first call.
 *
 *----------------------------------------------------------------------
 */

double
Ns_DRand(void)
{
    if (!initialized) {
        Ns_CsEnter(&lock);
        if (!initialized) {
            unsigned long seed[1];

            GenSeeds(seed, 1);
#if defined(HAVE_DRAND48)
            srand48((long) seed[0]);
#elif defined(HAVE_RANDOM)
            srandom((unsigned int) seed[0]);
#else
            srand((unsigned int) seed[0]);
#endif
            initialized = NS_TRUE;
        }
        Ns_CsLeave(&lock);
    }
#if defined(HAVE_ARC4RANDOM)
    return ((double)(arc4random() % (unsigned)RAND_MAX) / ((double)RAND_MAX + 1.0));
#elif defined(HAVE_DRAND48)
    return drand48();
#elif defined(HAVE_RANDOM)
    return ((double) random() / ((double)LONG_MAX + 1.0));
#else
    return ((double) rand() / ((double)RAND_MAX + 1.0));
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * GenSeeds --
 *
 *      Calculate an array of random seeds.
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
GenSeeds(unsigned long seeds[], int nseeds)
{
    Ns_Thread thr;

    Ns_Log(Notice, "random: generating %d seed%s", nseeds,
        nseeds == 1 ? NS_EMPTY_STRING : "s");
    Ns_CsEnter(&lock);
    Ns_SemaInit(&sema, 0);
    fRun = NS_TRUE;
    Ns_ThreadCreate(CounterThread, NULL, 0, &thr);
    while (nseeds-- > 0) {
        seeds[nseeds] = TrueRand();
    }
    fRun = NS_FALSE;
    Ns_SemaPost(&sema, 1);
    Ns_ThreadJoin(&thr, NULL);
    Ns_SemaDestroy(&sema);
    Ns_CsLeave(&lock);
}

/*
 *----------------------------------------------------------------------
 *
 * CounterThread --
 *
 *      Generate a random seed.  This routine runs as a separate thread where
 *      it increments a counter some indeterminate number of times.  The
 *      assumption is that this thread runs for a sufficiently long time to be
 *      preempted an arbitrary number of times by the kernel threads
 *      scheduler.
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
CounterThread(void *UNUSED(arg))
{
    while (fRun) {
        Ns_SemaWait(&sema);
        if (fRun) {
            while (fCount) {
                counter++;
            }
        }
    }
}

/*
 *==========================================================================
 * AT&T Seed Generation Code
 *==========================================================================
 *
 * The authors of this software are Don Mitchell and Matt Blaze.
 *              Copyright (c) 1995 by AT&T.
 * Permission to use, copy, and modify this software without fee
 * is hereby granted, provided that this entire notice is included in
 * all copies of any software which is or includes a copy or
 * modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * This software may be subject to United States export controls.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHORS NOR AT&T MAKE ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 */

#define MSEC_TO_COUNT 31  /* Duration of thread counting in milliseconds. */
#define ROULETTE_PRE_ITERS 10

static unsigned long
TrueRand(void)
{
    int i;

    for (i = 0; i < ROULETTE_PRE_ITERS; i++) {
        (void) Roulette();
    }
    return Roulette();
}

static unsigned long
Roulette(void)
{
    static unsigned long ocount = 0u, randbuf = 0u;
    struct timeval tv;

    counter = 0u;
    fCount = NS_TRUE;
    Ns_SemaPost(&sema, 1);
    tv.tv_sec =  0;
    tv.tv_usec = MSEC_TO_COUNT * 1000;
    select(0, NULL, NULL, NULL, &tv);
    fCount = NS_FALSE;
    counter ^= (counter >> 3) ^ (counter >> 6) ^ (ocount);
    counter &= 0x7u;
    ocount = counter;
    randbuf = (randbuf<<3) ^ counter;
    return randbuf;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
