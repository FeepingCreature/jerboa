CC=gcc
CFLAGS=-std=gnu11 -g -I. -Wall -Werror -Wfatal-errors -ljemalloc
DEPS=$(shell find . -name \*.h)
TESTOBJS=test.o object.o parser.o vm/runtime.o vm/call.o vm/builder.o vm/dump.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

test: $(TESTOBJS)
	$(CC) -o test $(TESTOBJS) $(CFLAGS)

.PHONY: clean
clean:
	rm $(shell find -name \*.o)
