# AuraIO C Examples
#
# Builds C examples using either:
# - Installed library (via pkg-config) - like real users would
# - Local development build (../lib) - for development
#
# Auto-detects which to use. Force local: make USE_LOCAL=1

CC = gcc
PKG_CONFIG ?= pkg-config

# Base compiler flags
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -O2

# Detect if libauraio is installed (unless USE_LOCAL=1)
ifndef USE_LOCAL
    HAS_AURAIO := $(shell $(PKG_CONFIG) --exists libauraio 2>/dev/null && echo yes)
else
    HAS_AURAIO := no
endif

# Configure flags based on installation status
ifeq ($(HAS_AURAIO),yes)
    $(info Building with installed libauraio (pkg-config))
    CFLAGS += $(shell $(PKG_CONFIG) --cflags libauraio)
    LDFLAGS = $(shell $(PKG_CONFIG) --libs libauraio)
    DEPS =
else
    $(info Building with local libauraio (../../core/lib))
    CFLAGS += -I../../core/include
    LDFLAGS = -L../../core/lib -lauraio -luring -lpthread -Wl,-rpath,'$$ORIGIN/../../core/lib'
    DEPS = ../../core/lib/libauraio.so
endif

# All C examples
EXAMPLES = quickstart simple_read bulk_reader write_modes cancel_request \
           custom_config vectored_io registered_buffers file_copy log_handler

all: $(EXAMPLES)

# Pattern rule for building examples
%: %.c $(DEPS)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(EXAMPLES)

help:
	@echo "AuraIO C Examples"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build all C examples"
	@echo "  make <name>       - Build specific example (e.g., make quickstart)"
	@echo "  make clean        - Remove built examples"
	@echo "  make USE_LOCAL=1  - Force local build"
	@echo ""
	@echo "Examples: $(EXAMPLES)"

.PHONY: all clean help
