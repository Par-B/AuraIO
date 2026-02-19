# aura-hash - parallel file checksum tool powered by AuraIO
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -I../../engine/include
LDFLAGS = -lpthread -luring -lcrypto -lm

BIN = ../bin/aura-hash
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): aura-hash.o $(PARENT_LIB)
	@mkdir -p ../bin
	$(CC) aura-hash.o $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

clean:
	rm -f aura-hash.o $(BIN)

.PHONY: all clean
