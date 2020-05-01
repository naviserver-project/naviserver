
/*
 * To enable OpenSSL (for both core nsd.exe and nsssl.dll), you must
 * define HAVE_OPENSSL_EVP_H in TWO files in "naviserver/include/":
 *   Makefile.win32 and nsconfig-win32.h
 * I tried many other approaches but could not get the build to work
 * any other way.  --atp@piskorski.com, 2020/05/01 12:17 EDT
 */

/* Needed for SSL support on Windows: */
#define HAVE_STDINT_H 1
#define HAVE_OPENSSL_EVP_H 1
