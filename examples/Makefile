# AuraIO Examples Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -std=c11 -O2 -I../include
CXXFLAGS = -Wall -Wextra -std=c++20 -O2 -I../include -fcoroutines
LDFLAGS = -L../lib -lauraio -luring -lpthread
RPATH = -Wl,-rpath,'$$ORIGIN/../../lib'

# C examples
C_EXAMPLES = C/quickstart C/simple_read C/bulk_reader C/write_modes C/cancel_request

# C++ examples
CPP_EXAMPLES = cpp/quickstart cpp/simple_read cpp/bulk_reader cpp/write_modes cpp/coroutine_copy

all: $(C_EXAMPLES)

# Build all C++ examples
cpp-examples: $(CPP_EXAMPLES)

# C examples
C/quickstart: C/quickstart.c ../lib/libauraio.so
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

C/simple_read: C/simple_read.c ../lib/libauraio.so
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

C/bulk_reader: C/bulk_reader.c ../lib/libauraio.so
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

C/write_modes: C/write_modes.c ../lib/libauraio.so
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

C/cancel_request: C/cancel_request.c ../lib/libauraio.so
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

# C++ examples
cpp/quickstart: cpp/quickstart.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

cpp/simple_read: cpp/simple_read.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

cpp/bulk_reader: cpp/bulk_reader.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

cpp/write_modes: cpp/write_modes.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

cpp/coroutine_copy: cpp/coroutine_copy.cpp ../lib/libauraio.so
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(RPATH)

clean:
	rm -f $(C_EXAMPLES) $(CPP_EXAMPLES)

.PHONY: all cpp-examples clean
