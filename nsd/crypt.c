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

#include "nsd.h"

#if !defined(HAVE_CRYPTR) && defined(__linux)
static Ns_Mutex lock = NULL;
#endif


#if defined(HAVE_CRYPTR)

char *
Ns_Encrypt(const char *pw, const char *salt, char iobuf[])
{
    char *enc;
    struct crypt_data data;

    NS_NONNULL_ASSERT(pw != NULL);
    NS_NONNULL_ASSERT(salt != NULL);
    NS_NONNULL_ASSERT(iobuf != NULL);
    
    data.initialized = 0;
    enc = crypt_r(pw, salt, &data);
    
    if (enc == NULL) {
	*iobuf = '\0';
    } else {
	strcpy(iobuf, enc);
    }
    
    return iobuf;
}

#elif defined(__linux)
/*
 * It seems that not every version of crypt() is compatible. We see different
 * results e.g. when we use crypt under Mac OS X for the crypted strings in
 * the regression test.
 */
char *
Ns_Encrypt(const char *pw, const char *salt, char iobuf[])
{
    char *enc;

    NS_NONNULL_ASSERT(pw != NULL);
    NS_NONNULL_ASSERT(salt != NULL);
    NS_NONNULL_ASSERT(iobuf != NULL);
    
    Ns_MutexLock(&lock);
    enc = crypt(pw, salt);
    Ns_MutexUnlock(&lock);

    if (enc == NULL) {
 	*iobuf = 0;
     } else {
	strcpy(iobuf, enc);
     }
     return iobuf;
 }

#else

/*
 * This program implements the Proposed Federal Information Processing Data
 * Encryption Standard. See Federal Register, March 17, 1975 (40FR12134)
 */

/*
 * Initial permutation,
 */
static const int IP[] = {
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7,
};

/*
 * Final permutation, FP = IP^(-1)
 */
static const int FP[] = {
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9, 49, 17, 57, 25,
};

/*
 * Permuted-choice 1 from the key bits to yield C and D. Note that bits
 * 8,16... are left out: They are intended for a parity check.
 */
static const int PC1_C[] = {
    57, 49, 41, 33, 25, 17, 9,
    1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27,
    19, 11, 3, 60, 52, 44, 36,
};

static const int PC1_D[] = {
    63, 55, 47, 39, 31, 23, 15,
    7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29,
    21, 13, 5, 28, 20, 12, 4,
};

/*
 * Sequence of shifts used for the key schedule.
 */
static const unsigned int shifts[] = {
    1u, 1u, 2u, 2u, 2u, 2u, 2u, 2u, 1u, 2u, 2u, 2u, 2u, 2u, 2u, 1u,
};

/*
 * Permuted-choice 2, to pick out the bits from the CD array that generate
 * the key schedule.
 */
static const int PC2_C[] = {
    14, 17, 11, 24, 1, 5,
    3, 28, 15, 6, 21, 10,
    23, 19, 12, 4, 26, 8,
    16, 7, 27, 20, 13, 2,
};

static const int PC2_D[] = {
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32,
};

/*
 * The following structure maitains the key schedule.
 */

struct sched {
    /*
     * The C and D arrays used to calculate the key schedule.
     */

    unsigned char C[28];
    unsigned char D[28];

    /*
     * The key schedule. Generated from the key.
     */
    unsigned char KS[16][48];

    /*
     * The E bit-selection table.
     */
    unsigned char E[48];
};

static const int e[] = {
    32, 1, 2, 3, 4, 5,
    4, 5, 6, 7, 8, 9,
    8, 9, 10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32, 1,
};

/*
 * Locally defined functions
 */
static void setkey_private(struct sched *sp, const unsigned char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void encrypt_private(const struct sched *sp, unsigned char *block, int edflag)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Set up the key schedule from the key.
 */

static void
setkey_private(struct sched *sp, const unsigned char *key)
{
    register int    i;

    NS_NONNULL_ASSERT(sp != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    /*
     * First, generate C and D by permuting the key.  The low order bit of
     * each 8-bit char is not used, so C and D are only 28 bits apiece.
     */
    for (i = 0; i < 28; i++) {
      sp->C[i] = key[PC1_C[i] - 1];
      sp->D[i] = key[PC1_D[i] - 1];
    }

    /*
     * To generate Ki, rotate C and D according to schedule and pick up a
     * permutation using PC2.
     */
    for (i = 0; i < 16; i++) {
        register unsigned int k;
        register int j;

        /*
         * rotate.
         */
        for (k = 0u; k < shifts[i]; k++) {
            unsigned char t = sp->C[0];
            for (j = 0; j < 28 - 1; j++) {
                sp->C[j] = sp->C[j + 1];
	    }
            sp->C[27] = t;
            t = sp->D[0];
            for (j = 0; j < 28 - 1; j++) {
                sp->D[j] = sp->D[j + 1];
	    }
            sp->D[27] = t;
        }

        /*
         * get Ki. Note C and D are concatenated.
         */
        for (j = 0; j < 24; j++) {
            sp->KS[i][j] = sp->C[PC2_C[j] - 1];
            sp->KS[i][j + 24] = sp->D[PC2_D[j] - 29];
        }
    }

    for (i = 0; i < 48; i++) {
        sp->E[i] = (unsigned char)e[i];
    }
}

/*
 * The 8 selection functions. For some reason, they give a 0-origin index,
 * unlike everything else.
 */
static const unsigned int S[8][64] = {
    { 14u,  4u, 13u,  1u,  2u, 15u, 11u,  8u,  3u, 10u,  6u, 12u,  5u,  9u,  0u,  7u,
       0u, 15u,  7u,  4u, 14u,  2u, 13u,  1u, 10u,  6u, 12u, 11u,  9u,  5u,  3u,  8u,
       4u,  1u, 14u,  8u, 13u,  6u,  2u, 11u, 15u, 12u,  9u,  7u,  3u, 10u,  5u,  0u,
      15u, 12u,  8u,  2u,  4u,  9u,  1u,  7u,  5u, 11u,  3u, 14u, 10u,  0u,  6u, 13u },

    { 15u,  1u,  8u, 14u,  6u, 11u,  3u,  4u,  9u,  7u,  2u, 13u, 12u,  0u,  5u, 10u,
       3u, 13u,  4u,  7u, 15u,  2u,  8u, 14u, 12u,  0u,  1u, 10u,  6u,  9u, 11u,  5u,
       0u, 14u,  7u, 11u, 10u,  4u, 13u,  1u,  5u,  8u, 12u,  6u,  9u,  3u,  2u, 15u,
      13u,  8u, 10u,  1u,  3u, 15u,  4u,  2u, 11u,  6u,  7u, 12u,  0u,  5u, 14u, 9u },

    { 10u,  0u,  9u, 14u,  6u,  3u, 15u,  5u,  1u, 13u, 12u,  7u, 11u,  4u,  2u,  8u,
      13u,  7u,  0u,  9u,  3u,  4u,  6u, 10u,  2u,  8u,  5u, 14u, 12u, 11u, 15u,  1u,
      13u,  6u,  4u,  9u,  8u, 15u,  3u,  0u, 11u,  1u,  2u, 12u,  5u, 10u, 14u,  7u,
       1u, 10u, 13u,  0u,  6u,  9u,  8u,  7u,  4u, 15u, 14u,  3u, 11u,  5u,  2u, 12u },

    {  7u, 13u, 14u,  3u,  0u,  6u,  9u, 10u,  1u,  2u,  8u,  5u, 11u, 12u,  4u, 15u,
      13u,  8u, 11u,  5u,  6u, 15u,  0u,  3u,  4u,  7u,  2u, 12u,  1u, 10u, 14u,  9u,
      10u,  6u,  9u,  0u, 12u, 11u,  7u, 13u, 15u,  1u,  3u, 14u,  5u,  2u,  8u,  4u,
       3u, 15u,  0u,  6u, 10u,  1u, 13u,  8u,  9u,  4u,  5u, 11u, 12u,  7u,  2u, 14u },

    {  2u, 12u,  4u,  1u,  7u, 10u, 11u,  6u,  8u,  5u,  3u, 15u, 13u,  0u, 14u,  9u,
      14u, 11u,  2u, 12u,  4u,  7u, 13u,  1u,  5u,  0u, 15u, 10u,  3u,  9u,  8u,  6u,
       4u,  2u,  1u, 11u, 10u, 13u,  7u,  8u, 15u,  9u, 12u,  5u,  6u,  3u,  0u, 14u,
      11u,  8u, 12u,  7u,  1u, 14u,  2u, 13u,  6u, 15u,  0u,  9u, 10u,  4u,  5u, 3u },

    { 12u,  1u, 10u, 15u,  9u,  2u,  6u,  8u,  0u, 13u,  3u,  4u, 14u,  7u,  5u, 11u,
      10u, 15u,  4u,  2u,  7u, 12u,  9u,  5u,  6u,  1u, 13u, 14u,  0u, 11u,  3u,  8u,
       9u, 14u, 15u,  5u,  2u,  8u, 12u,  3u,  7u,  0u,  4u, 10u,  1u, 13u, 11u,  6u,
       4u,  3u,  2u, 12u,  9u,  5u, 15u, 10u, 11u, 14u,  1u,  7u,  6u,  0u,  8u, 13u },

    {  4u, 11u,  2u, 14u, 15u,  0u,  8u, 13u,  3u, 12u,  9u,  7u,  5u, 10u,  6u,  1u,
      13u,  0u, 11u,  7u,  4u,  9u,  1u, 10u, 14u,  3u,  5u, 12u,  2u, 15u,  8u,  6u,
       1u,  4u, 11u, 13u, 12u,  3u,  7u, 14u, 10u, 15u,  6u,  8u,  0u,  5u,  9u,  2u,
       6u, 11u, 13u,  8u,  1u,  4u, 10u,  7u,  9u,  5u,  0u, 15u, 14u,  2u,  3u, 12u },

    { 13u,  2u,  8u,  4u,  6u, 15u, 11u,  1u, 10u,  9u,  3u, 14u,  5u,  0u, 12u,  7u,
       1u, 15u, 13u,  8u, 10u,  3u,  7u,  4u, 12u,  5u,  6u, 11u,  0u, 14u,  9u,  2u,
       7u, 11u,  4u,  1u,  9u, 12u, 14u,  2u,  0u,  6u, 10u, 13u, 15u,  3u,  5u,  8u,
       2u,  1u, 14u,  7u,  4u, 10u,  8u, 13u, 15u, 12u,  9u,  0u,  3u,  5u,  6u, 11u}, 
};

/*
 * P is a permutation on the selected combination of the current L and key.
 */
static const int P[] = {
    16, 7, 20, 21,
    29, 12, 28, 17,
    1, 15, 23, 26,
    5, 18, 31, 10,
    2, 8, 24, 14,
    32, 27, 3, 9,
    19, 13, 30, 6,
    22, 11, 4, 25,
};

/*
 * The payoff: encrypt a block.
 */

static void
encrypt_private(const struct sched *sp, unsigned char *block, int edflag)
{
    /*
     * The current block, divided into 2 halves.
     */
    unsigned char L[64], *R = L + 32;
    unsigned char tempL[32];
    unsigned char f[32];

    /*
     * The combination of the key and the input, before selection.
     */
    unsigned char preS[48];

    int             i, ii;
    register int    j;

    NS_NONNULL_ASSERT(sp != NULL);
    NS_NONNULL_ASSERT(block != NULL);

    /*
     * First, permute the bits in the input
     */
    for (j = 0; j < 64; j++) {
        L[j] = block[IP[j] - 1];
    }

    /*
     * Perform an encryption operation 16 times.
     */
    for (ii = 0; ii < 16; ii++) {

        /*
         * Set direction
         */
	if (edflag != 0) {
            i = 15 - ii;
	} else {
            i = ii;
	}

        /*
         * Save the R array, which will be the new L.
         */
        for (j = 0; j < 32; j++) {
            tempL[j] = R[j];
	}

        /*
         * Expand R to 48 bits using the E selector; exclusive-or with the
         * current key bits.
         */
        for (j = 0; j < 48; j++) {
            preS[j] = R[sp->E[j] - 1u] ^ sp->KS[i][j];
	}

        /*
         * The pre-select bits are now considered in 8 groups of 6 bits each.
         * The 8 selection functions map these 6-bit quantities into 4-bit
         * quantities and the results permuted to make an f(R, K). The
         * indexing into the selection functions is peculiar; it could be
         * simplified by rewriting the tables.
         */
        for (j = 0; j < 8; j++) {
            register unsigned int k;
            register int          t;

            t = 6 * j;
            k = S[j][
                UCHAR(preS[t] << 5) +
                UCHAR(preS[t + 1] << 3) +
                UCHAR(preS[t + 2] << 2) +
                UCHAR(preS[t + 3] << 1) +
                (preS[t + 4]     ) +
                UCHAR(preS[t + 5] << 4)];
            t = 4 * j;
            assert(t < (32-3));

            f[t    ] = UCHAR((k >> 3) & 1u);
            f[t + 1] = UCHAR((k >> 2) & 1u);
            f[t + 2] = UCHAR((k >> 1) & 1u);
            f[t + 3] = UCHAR((k     ) & 1u);
        }

        /*
         * The new R is L ^ f(R, K). The f here has to be permuted first,
         * though.
         */
        for (j = 0; j < 32; j++) {
            R[j] = L[j] ^ f[P[j] - 1];
	}

        /*
         * Finally, the new L (the original R) is copied back.
         */
        for (j = 0; j < 32; j++) {
            L[j] = tempL[j];
	}
    }

    /*
     * The output L and R are reversed.
     */
    for (j = 0; j < 32; j++) {
        register unsigned char t = L[j];
        L[j] = R[j];
        R[j] = t;
    }

    /*
     * The final output gets the inverse permutation of the very original.
     */
    for (j = 0; j < 64; j++) {
        block[j] = L[FP[j] - 1];
    }
}


char *
Ns_Encrypt(const char *pw, const char *salt, char iobuf[])
{
    register size_t i;
    register int    j;
    unsigned char   c;
    unsigned char   block[66];
    struct sched    s;

    NS_NONNULL_ASSERT(pw != NULL);
    NS_NONNULL_ASSERT(salt != NULL);
    NS_NONNULL_ASSERT(iobuf != NULL);

    for (i = 0u; i < 66u; i++) {
        block[i] = UCHAR('\0');
    }
    for (i = 0u, c = UCHAR(*pw); c != UCHAR('\0') && i < 64u; pw++, c = UCHAR(*pw)) {
	for (j = 0; j < 7; j++, i++) {
            assert(i < sizeof(block));
            block[i] = (c >> (6 - j)) & 1u;
	}
        i++;
    }

    setkey_private(&s, block);

    for (i = 0u; i < 66u; i++) {
        block[i] = UCHAR('\0');
    }

    for (i = 0u; i < 2u; i++) {
        c = UCHAR(*salt++);
        iobuf[i] = (char)c;
        if (c > UCHAR('Z')) {
            c -= 6u;
	}
        if (c > UCHAR('9')) {
            c -= 7u;
	}
        c -= UCHAR('.');
        for (j = 0; j < 6; j++) {
            if ((c >> j) & 1u) {
                unsigned char temp = s.E[6u * i + j];
                s.E[6u * i + j] = s.E[6u * i + j + 24];
                s.E[6u * i + j + 24] = temp;
            }
        }
    }

    for (i = 0u; i < 25u; i++) {
        encrypt_private(&s, block, 0);
    }

    for (i = 0u; i < 11u; i++) {
        c = UCHAR('\0');
        for (j = 0; j < 6; j++) {
            c <<= 1;
            c |= block[6u * i + j];
        }
        c += UCHAR('.');
        if (c > UCHAR('9')) {
            c += 7u;
	}
        if (c > UCHAR('Z')) {
            c += 6u;
	}
        iobuf[i + 2u] = (char)c;
    }
    iobuf[i + 2u] = '\0';
    if (iobuf[1] == '\0') {
        iobuf[1] = iobuf[0];
    }

    return (iobuf);
}

#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
