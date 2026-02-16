# BFFIO - Better Faster FIO
# FIO-compatible I/O benchmark powered by AuraIO
#
# Build:    make
# Test:     make test
# Clean:    make clean
# Baseline: make baseline

CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-declarations \
         -std=c11 -O2 -I../../engine/include
LDFLAGS = -lpthread -luring -lm

SRC = main.c job_parser.c workload.c stats.c output.c
OBJ = $(SRC:.c=.o)
BIN = ../bin/BFFIO
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): $(OBJ) $(PARENT_LIB)
	@mkdir -p ../bin
	$(CC) $(OBJ) $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

test: $(BIN)
	BIN=$(BIN) ./test_BFFIO.sh

baseline: $(BIN)
	BIN=$(BIN) ./run_baseline.sh --standard

baseline-quick: $(BIN)
	BIN=$(BIN) ./run_baseline.sh --quick

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean test baseline baseline-quick
