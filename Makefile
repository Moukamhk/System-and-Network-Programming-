CC     = gcc
CFLAGS = -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
DBGF   = $(CFLAGS) -DDEBUG

.PHONY: all debug clean

all: lab1_daemon

debug: lab1_debug

lab1_daemon: lab1_daemon.c lab1_common.h
	$(CC) $(CFLAGS) -o $@ lab1_daemon.c

lab1_debug: lab1_daemon.c lab1_common.h
	$(CC) $(DBGF) -o $@ lab1_daemon.c

clean:
	rm -f lab1_daemon lab1_debug
