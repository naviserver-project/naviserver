/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

#include "nsthread.h"

static void Check(uint64_t value);

static void
Check(uint64_t value)
{
    char guarded[NS_UINT64_HEX_SPACE + 2];
    char expected[NS_UINT64_HEX_SPACE];
    int  length, expectedLength;

    memset(guarded, '!', sizeof(guarded));
    expectedLength = snprintf(expected, sizeof(expected), "%" PRIx64, value);
    length = ns_uint64tohex(guarded + 1, value);
    if (length != expectedLength || strcmp(guarded + 1, expected) != 0
        || guarded[0] != '!' || guarded[length + 2] != '!') {
        fprintf(stderr, "hex conversion failed for %" PRIu64 "\n", value);
        exit(EXIT_FAILURE);
    }
}

int
main(void)
{
    uint64_t value = UINT64_C(0x123456789abcdef0);
    unsigned int i;

    Check(0u);
    Check(UINT64_MAX);
    for (i = 0u; i < 64u; ++i) {
        uint64_t boundary = UINT64_C(1) << i;

        Check(boundary - 1u);
        Check(boundary);
        Check(boundary + 1u);
    }
    for (i = 0u; i < 100000u; ++i) {
        value ^= value << 13u;
        value ^= value >> 7u;
        value ^= value << 17u;
        Check(value);
    }
    printf("hex conversion: boundary, random and buffer-guard tests passed\n");
    return EXIT_SUCCESS;
}
