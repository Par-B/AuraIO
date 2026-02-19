# atree - AuraIO tree with per-file stats
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -I../../engine/include
LDFLAGS = -lpthread -luring

BIN = ../bin/atree
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): atree.o $(PARENT_LIB)
	@mkdir -p ../bin
	$(CC) atree.o $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

clean:
	rm -f atree.o $(BIN)

.PHONY: all clean
