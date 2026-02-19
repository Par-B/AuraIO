# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AuraIO Contributors
#
# sspa - Simple Storage Performance Analyzer powered by AuraIO
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -I../../engine/include
LDFLAGS = -lpthread -luring -lm

BIN = ../bin/sspa
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): sspa.o $(PARENT_LIB)
	@mkdir -p ../bin
	$(CC) sspa.o $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

clean:
	rm -f sspa.o $(BIN)

.PHONY: all clean
