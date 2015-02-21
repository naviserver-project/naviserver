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
MODOBJS     = nsssl.o

#MODLIBS = -L/usr/local/ssl/lib -Wl,-rpath,/usr/local/ssl/lib
MODLIBS  += -lssl -lcrypto

include  $(NAVISERVER)/include/Makefile.module

dhparams.h:
	openssl dhparam -C -2 -noout 512 >> dhparams.h
	openssl dhparam -C -2 -noout 1024 >> dhparams.h

nsssl.o: dhparams.h
