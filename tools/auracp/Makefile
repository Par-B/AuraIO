# auracp - async pipelined file copy powered by AuraIO
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -I../../engine/include
LDFLAGS = -lpthread -luring -lm

BIN = ../bin/auracp
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): auracp.o $(PARENT_LIB)
	@mkdir -p ../bin
	$(CC) auracp.o $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

clean:
	rm -f auracp.o $(BIN)

.PHONY: all clean
