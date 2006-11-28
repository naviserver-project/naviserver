ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Module name
#
MOD      =  nsssl.so

#
# Objects to build.
#
OBJS     = nsssl.o

MODLIBS  += -lssl -lcrypto

include  $(NAVISERVER)/include/Makefile.module

