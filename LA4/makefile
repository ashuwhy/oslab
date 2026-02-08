# Makefile for Snake Ludo
# CS39002 Operating Systems Laboratory
# Author: Ashutosh Sharma
# Roll: 23CS10005

CC = gcc
CFLAGS = -Wall -g

# Target executables
TARGETS = ludo board players

.PHONY: all clean

all: $(TARGETS)

ludo: ludo.c
	$(CC) $(CFLAGS) -o ludo ludo.c

board: board.c
	$(CC) $(CFLAGS) -o board board.c

players: players.c
	$(CC) $(CFLAGS) -o players players.c

clean:
	rm -f $(TARGETS)

# Run interactive mode with 4 players
run: all
	./ludo 4

# Run interactive mode with 4 players (explicit)
run-interactive: all
	./ludo 4

# Run autoplay mode with 4 players (1 second delay)
run-auto: all
	./ludo 4 autoplay 1000

# Clean up any leftover shared memory (for debugging)
clean-shm:
	@echo "Cleaning up shared memory segments..."
	@ipcs -m | grep -E "0x0000(1234|5678)" | awk '{print $$2}' | while read id; do ipcrm -m $$id; done
	@echo "Done."
