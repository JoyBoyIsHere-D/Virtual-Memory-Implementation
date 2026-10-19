CC = gcc
CFLAGS = -Wall -Wextra

all: master mmu process sched

master: Master.c
	$(CC) $(CFLAGS) -o master Master.c

mmu: MMU.c
	$(CC) $(CFLAGS) -o mmu MMU.c

process: process.c
	$(CC) $(CFLAGS) -o process process.c

sched: sched.c
	$(CC) $(CFLAGS) -o sched sched.c

clean:
	rm -f master mmu process sched result.txt

clean-ipc:
	ipcrm -a 2>/dev/null || true

.PHONY: all clean clean-ipc
