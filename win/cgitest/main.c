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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * This file is just for testing purposes under windows to check the
 * CGI interface.
 */

int
main(int argc, char **argv)
{
    int i;
    char buf[8192];

    if (strstr(argv[0], "nph-") != NULL) {
        printf("HTTP/1.0 200 OK\r\nServer: %s\r\n", argv[0]);
    }
    printf("Content-type: text/plain\r\n\r\n");
    puts("\nArgs:");
    for (i = 0; i < argc; ++i) {
        puts(argv[i]);
    }

    puts("\nEnvironment:");
    for (i = 0; _environ[i] != NULL; ++i) {
        printf("%s\n", _environ[i]);
    }

    puts("\nContent:");
    while ((i = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        fwrite(buf, 1, i, stdout);
    }
    return 0;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
