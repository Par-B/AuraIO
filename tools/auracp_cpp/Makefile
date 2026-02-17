# auracp_cpp - async pipelined file copy (C++ version) powered by AuraIO
#
# Build: make
# Clean: make clean

CXX = g++
CXXFLAGS = -Wall -Wextra -Wpedantic -std=c++20 -O2 -fcoroutines \
           -I../../engine/include
LDFLAGS = -lpthread -luring -lm

BIN = ../bin/auracp_cpp
PARENT_LIB = ../../engine/lib/libaura.a

all: $(BIN)

$(BIN): auracp.o $(PARENT_LIB)
	@mkdir -p ../bin
	$(CXX) auracp.o $(PARENT_LIB) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PARENT_LIB):
	$(MAKE) -C ../..

clean:
	rm -f auracp.o $(BIN)

.PHONY: all clean
