CC=gcc
CFLAGS=-Wall -Werror -Wextra -std=c11 -O2 -fomit-frame-pointer -march=native

.PHONY: all
all: mai

mai: args.o audio.o jack.o mai.o ptp.o rtp.o sap.o sock.o
	$(CC) $(CFLAGS) -pthread -o $@ $^ -lm -ljack -lsamplerate

.PHONY: clean
clean:
	rm -f mai *.o
