TARGET_EXEC ?= ashdec

CC = gcc
CFLAGS = -Wall -O3
DBGFLAGS = -Wall -g

all:
	$(CC) main.c $(CFLAGS) -o $(TARGET_EXEC)

debug:
	$(CC) main.c $(DBGFLAGS) -o $(TARGET_EXEC)

.PHONY: clean

clean:
	rm $(TARGET_EXEC)