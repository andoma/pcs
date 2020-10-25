

SRCS=pcs.c main.c

CFLAGS=-g -O2 -Wall -Werror -fsanitize=address -Wmissing-prototypes

pcs: ${SRCS} Makefile
	$(CC) ${CFLAGS} -o $@ ${SRCS} -lpthread
