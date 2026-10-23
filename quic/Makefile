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

# Queue-level tests in a QUIC-enabled build; no network or TLS connections.
.PHONY: check-shared-send clean-shared-send
check-shared-send: shared-send-test$(EXEEXT)
	LD_LIBRARY_PATH="$(CURDIR)/../nsthread:$${LD_LIBRARY_PATH}" DYLD_LIBRARY_PATH="$(CURDIR)/../nsthread:$${DYLD_LIBRARY_PATH}" ./shared-send-test$(EXEEXT)

shared-send-test$(EXEEXT): shared-send-test.o shared.o chunk.o
	$(CC) $(LDFLAGS) -o $@ shared-send-test.o shared.o chunk.o $(filter-out -lnsd,$(CCLIBS)) $(CCRPATH)

shared-send-test.o: shared-send-test.c shared.h chunk.h

clean: clean-shared-send
clean-shared-send:
	$(RM) shared-send-test$(EXEEXT) shared-send-test.o

