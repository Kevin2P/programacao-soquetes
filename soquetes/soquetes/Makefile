CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread

all: server client tester

server: server.c net.c proto.c estado.c net.h proto.h estado.h common.h
	$(CC) $(CFLAGS) -o $@ server.c net.c proto.c estado.c

client: client.c net.c proto.c net.h proto.h common.h
	$(CC) $(CFLAGS) -o $@ client.c net.c proto.c

tester: tester.c net.c proto.c net.h proto.h common.h
	$(CC) $(CFLAGS) -o $@ tester.c net.c proto.c

test: all
	./tests/run_tests.sh

bench: all
	./tests/bench.sh

clean:
	rm -f server client tester

.PHONY: all test bench clean
