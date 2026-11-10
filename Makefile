# Makefile for the banking system.
#   make          build everything (server, client, tools, tests)
#   make test     build and run ALL tests (ACID core tests + server tests)
#   make clean    remove build output

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -D_GNU_SOURCE -Iinclude
LDLIBS  = -lcrypt -pthread

# 1. core library: files, locks, journal, banking rules (no networking)
CORE_SRC = src/db.c src/journal.c src/auth.c \
           src/bank_core.c src/bank_customer.c src/bank_staff.c src/bank_lists.c src/bank_names.c
# 2. server: supervisor, event loop, worker, helpers
SERVER_SRC = src/proto.c src/server/net.c src/server/log.c src/server/dispatch.c \
             src/server/worker.c src/server/loop.c src/server/supervisor.c
# 3. client library
CLIENT_SRC = src/client/client.c

CORE_OBJ   = $(CORE_SRC:src/%.c=build/%.o)
SERVER_OBJ = $(SERVER_SRC:src/%.c=build/%.o)
CLIENT_OBJ = $(CLIENT_SRC:src/%.c=build/%.o) build/proto.o build/server/net.o

CORE_TESTS   = bin/test_atomicity bin/test_consistency bin/test_isolation bin/test_durability bin/test_lists bin/test_crash_ops
SERVER_TESTS = bin/test_server_basic bin/test_server_crash bin/test_server_sessions

all: bin/bankctl bin/bank_server bin/bank_client bin/bank_cmd $(CORE_TESTS) $(SERVER_TESTS)

build/%.o: src/%.c include/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

bin/bankctl: tools/bankctl.c $(CORE_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $< $(CORE_OBJ) -o $@ $(LDLIBS)

bin/bank_server: tools/bank_server.c $(CORE_OBJ) $(SERVER_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $< $(CORE_OBJ) $(SERVER_OBJ) -o $@ $(LDLIBS)

bin/bank_client: tools/bank_client.c $(CLIENT_OBJ) $(CORE_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $< $(CLIENT_OBJ) $(CORE_OBJ) -o $@ $(LDLIBS)

bin/bank_cmd: tools/bank_cmd.c $(CLIENT_OBJ) $(CORE_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $< $(CLIENT_OBJ) $(CORE_OBJ) -o $@ $(LDLIBS)

# core tests: test + core library
bin/test_atomicity bin/test_consistency bin/test_isolation bin/test_durability bin/test_lists bin/test_crash_ops: bin/test_%: tests/test_%.c tests/test_helpers.h $(CORE_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -Itests $< $(CORE_OBJ) -o $@ $(LDLIBS)

# server tests: test + everything
$(SERVER_TESTS): bin/test_%: tests/test_%.c tests/test_helpers.h $(CORE_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -Itests $< $(CORE_OBJ) build/proto.o build/server/net.o build/server/log.o build/client/client.o -o $@ $(LDLIBS)

test: all
	./tests/run_tests.sh

clean:
	rm -rf build bin

.PHONY: all test clean
