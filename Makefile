CFLAGS=-g -I. 

OBJS=socktest.o

#  Change definitions below to compile for Solaris
#socktest:${OBJS}
#	gcc -o socktest ${OBJS} -lc -lsocket -lnsl

socktest:${OBJS}
	gcc -o socktest ${OBJS} -lc -lreadline -lncurses 

install:

all:socktest
