# gowithme.club playtime tracker for a jailbroken PS5.
#
#   make            build build/gowithme.club_v<version>.elf and a copy in dist/
#   make test       host tests, no console and no SDK needed
#   make deploy     send to elfldr on the console: PS5_HOST=<ps5 ip> make deploy
#
# Needs https://github.com/ps5-payload-dev/sdk (binary release in ~/ps5-payload-sdk)
# and, on macOS, llvm@18 from Homebrew.

PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
LLVM_CONFIG     ?= /opt/homebrew/opt/llvm@18/bin/llvm-config
PS5_HOST        ?= ps5
PS5_PORT        ?= 9021

export LLVM_CONFIG

VERSION := $(shell sed -n 's/.*GWM_VERSION "\(.*\)".*/\1/p' src/version.h)

# Payload Manager shows the file as "gowithme.club" with version "v<version>",
# parsed from the _v<version> suffix.
ELF_NAME := gowithme.club_v$(VERSION).elf

CC      := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
DEPLOY  := $(PS5_PAYLOAD_SDK)/bin/prospero-deploy

SQLITE_DIR   := third_party/sqlite
# Single thread, no extensions, no WAL. Small databases are read as an in-memory
# copy (sqlite3_deserialize), the telemetry journal in place via the unix-none VFS.
SQLITE_FLAGS := -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
                -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_TEMP_STORE=3 \
                -DSQLITE_OMIT_WAL -DSQLITE_OMIT_SHARED_CACHE \
                -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DEPRECATED

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -Isrc -I$(SQLITE_DIR)
LIBS    := -lSceUserService -lSceSystemService

BUILD   := build
ELF     := $(BUILD)/$(ELF_NAME)
HEADERS := $(wildcard src/*.h)

.PHONY: all test deploy clean sdk-check

all: $(ELF)
	@mkdir -p dist
	@cp $(ELF) dist/$(ELF_NAME)
	@echo "-> dist/$(ELF_NAME)"

$(BUILD)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c | sdk-check
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(SQLITE_FLAGS) -c -o $@ $<

$(ELF): src/main.c $(HEADERS) $(BUILD)/sqlite3.o | sdk-check
	$(CC) $(CFLAGS) $(SQLITE_FLAGS) -o $@ src/main.c $(BUILD)/sqlite3.o $(LIBS)

sdk-check:
	@test -x $(CC) || { echo "no SDK in $(PS5_PAYLOAD_SDK), see README.md"; exit 1; }
	@test -x $(LLVM_CONFIG) || { echo "no llvm-config: brew install llvm@18"; exit 1; }

deploy: $(ELF)
	$(DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

# Pure-logic tests, built with the host cc. Run from the repository root:
# some tests read tests/fixtures/.
TESTS := json time http_config queue session trophy pair reply journal trophy2

test: $(BUILD)/tests/sqlite3.o
	@mkdir -p $(BUILD)/tests
	@set -e; for t in $(TESTS); do \
		cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -DGWM_HOST_TEST -Isrc \
		   -o $(BUILD)/tests/$$t tests/$${t}_test.c; \
		./$(BUILD)/tests/$$t; \
	done
	cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Isrc -I$(SQLITE_DIR) \
	   -o $(BUILD)/tests/journal_io tests/journal_io_test.c $(BUILD)/tests/sqlite3.o
	./$(BUILD)/tests/journal_io

$(BUILD)/tests/sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	@mkdir -p $(BUILD)/tests
	cc -O0 -w $(SQLITE_FLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
