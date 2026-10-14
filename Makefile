CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

.PHONY: all test sanitize clean
all: qcsh

qcsh: main.c
	$(CC) $(CFLAGS) main.c -o qcsh

test: qcsh
	python3 tests/test_shell.py
	python3 tests/test_interactive.py

sanitize:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer main.c -o qcsh-sanitize
	QCSH_BIN=$(CURDIR)/qcsh-sanitize python3 tests/test_shell.py

clean:
	rm -f qcsh qcsh-sanitize
