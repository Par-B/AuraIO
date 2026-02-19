# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AuraIO Contributors
#
# aura-check - simple storage performance analyzer powered by AuraIO
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -I../../engine/include
LDFLAGS = -lpthread -luring -lm

BIN = ../bin/aura-check
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): aura-check.o $(PARENT_LIB)
	@mkdir -p ../bin
	$(CC) aura-check.o $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

clean:
	rm -f aura-check.o $(BIN)

.PHONY: all clean
