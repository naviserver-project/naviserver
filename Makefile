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

dhparams.h:
	openssl dhparam -C -2 -noout 512 >> dhparams.h
	openssl dhparam -C -2 -noout 1024 >> dhparams.h

nsssl.o: dhparams.h
