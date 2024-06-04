TARGET_EXEC ?= ashdec

CC = gcc
CFLAGS = -Wall -O2
DBGFLAGS = -g

all:
	$(CC) main.c $(CFLAGS) -o $(TARGET_EXEC)

debug:
	$(CC) main.c $(CFLAGS) $(DBGFLAGS) -o $(TARGET_EXEC)

.PHONY: clean

clean:
	rm $(TARGET_EXEC)