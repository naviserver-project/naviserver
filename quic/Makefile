# Emacs mode:  -*-Makefile-*-

# This is the Linux/Unix specific file, do NOT put Microsoft Windows
# nmake stuff here.

#NGHTTP3_CFLAGS = -I/usr/local/nghttp3-latest/include
#NGHTTP3_LIBS   = -L/usr/local/nghttp3-latest/lib -lnghttp3

NGHTTP3_CFLAGS = -I/opt/local/include
NGHTTP3_LIBS   = -L/opt/local/lib -lnghttp3

MODNAME  = quic
MOD      = quic.so
MODOBJS  = quic.o chunk.o shared.o
HDRS     = shared.h chunk.h thread-affinity.h

include ../include/Makefile.build

MODLIBS  += -L../nsd $(NGHTTP3_LIBS)
CFLAGS   += $(NGHTTP3_CFLAGS)

quic.o: ../nsd/nsopenssl.h ../nsd/nsd.h ../nsd/nsatomic.h
