/*
 * Activate HAVE_OPENSSL_EVP_H for OpenSSL on Windows. This assumes
 * that openssl include files (e.g. openssl/evp.h) are on the include
 * path, and the lib files (e.g. libssl, libcrypto) are available for
 * linking. The locations are provided in include/Makefile.win32
 */
#define HAVE_OPENSSL_EVP_H 1

/*
 * Activate HAVE_ZLIB_H for using zlib on Windows. This assumes that
 * zlib.h and zconf.h can be found on the include path, and zlib*.lib
 * is available for linking.
 */
#define HAVE_ZLIB_H 1

#define HAVE_STDINT_H 1
#define HAVE_TIMEGM 1
