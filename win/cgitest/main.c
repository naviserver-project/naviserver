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
