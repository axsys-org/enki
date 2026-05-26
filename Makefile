BUILD_TYPE ?= debug
CC ?= cc
PREFIX ?= /usr/local
BUILD_DIR ?= build/$(BUILD_TYPE)
AR ?= ar

VALID_BUILD_TYPES := debug release asan ubsan tsan coverage

BASE_CPPFLAGS := -Iinclude -Itests/support -Itests/property/vendor/theft -isystem /opt/homebrew/include
BASE_CFLAGS := -std=c11 -MMD -MP

WARN_COMMON := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
	-Wmissing-prototypes -Wold-style-definition -Wnull-dereference -Wdouble-promotion -Werror

CC_VERSION := $(shell $(CC) --version 2>/dev/null)
IS_CLANG := $(findstring clang,$(CC_VERSION))
IS_GNU := $(findstring GCC,$(CC_VERSION))
WARN_CFLAGS := $(if $(or $(IS_CLANG),$(IS_GNU)),$(WARN_COMMON),)

HARDEN_CFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong

BUILD_CFLAGS_debug := -O0 -g3 -DDEBUG
BUILD_CFLAGS_release := -O2 -DNDEBUG $(HARDEN_CFLAGS)
BUILD_CFLAGS_asan := -O1 -g3 -fsanitize=address -fno-omit-frame-pointer $(HARDEN_CFLAGS)
BUILD_CFLAGS_ubsan := -O1 -g3 -fsanitize=undefined -fno-omit-frame-pointer $(HARDEN_CFLAGS)
BUILD_CFLAGS_tsan := -O1 -g3 -fsanitize=thread -fno-omit-frame-pointer $(HARDEN_CFLAGS)
BUILD_CFLAGS_coverage := -O1 -g3 --coverage $(HARDEN_CFLAGS)

BUILD_LDFLAGS_debug :=
BUILD_LDFLAGS_release :=
BUILD_LDFLAGS_asan := -fsanitize=address
BUILD_LDFLAGS_ubsan := -fsanitize=undefined
BUILD_LDFLAGS_tsan := -fsanitize=thread
BUILD_LDFLAGS_coverage := --coverage

ifeq ($(filter $(BUILD_TYPE),$(VALID_BUILD_TYPES)),)
$(error BUILD_TYPE must be one of $(VALID_BUILD_TYPES))
endif

CPPFLAGS_ALL := $(BASE_CPPFLAGS) $(CPPFLAGS)
CFLAGS_ALL := $(BASE_CFLAGS) $(WARN_CFLAGS) $(BUILD_CFLAGS_$(BUILD_TYPE)) $(CFLAGS)
LDFLAGS_ALL := $(BUILD_LDFLAGS_$(BUILD_TYPE)) $(LDFLAGS) -L/opt/homebrew/lib -lgmp -llmdb -lcrypto

SRC_DIR := src
INCLUDE_DIR := include
UNIT_DIR := tests/unit
PROPERTY_DIR := tests/property
FUZZ_DIR := tests/fuzz
VENDOR_THEFT_DIR := tests/property/vendor/theft

SRCS := $(wildcard $(SRC_DIR)/*.c)
HEADERS := $(wildcard $(INCLUDE_DIR)/enki/*.h)
TSAN_UNIT_SRCS := $(wildcard $(UNIT_DIR)/*_tsan.c)
UNIT_SRCS := $(filter-out $(TSAN_UNIT_SRCS),$(wildcard $(UNIT_DIR)/*.c))
PROPERTY_SRCS := $(wildcard $(PROPERTY_DIR)/*.c)
THEFT_SRCS := $(wildcard $(VENDOR_THEFT_DIR)/*.c)
FUZZ_SRCS := $(wildcard $(FUZZ_DIR)/*.c)

LIB_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
UNIT_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(UNIT_SRCS))
TSAN_UNIT_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(TSAN_UNIT_SRCS))
PROPERTY_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(PROPERTY_SRCS))
THEFT_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(THEFT_SRCS))
FUZZ_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(FUZZ_SRCS))
LIB := $(BUILD_DIR)/lib/libenki.a

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(BUILD_TYPE)-$(UNAME_S),tsan-Darwin)
ACTIVE_UNIT_BINS := $(TSAN_UNIT_BINS)
else
ACTIVE_UNIT_BINS := $(UNIT_BINS)
endif

TIDY_FILES := $(SRCS) $(UNIT_SRCS) $(PROPERTY_SRCS) $(FUZZ_SRCS) $(TSAN_UNIT_SRCS)
TIDY_FILES_ABS := $(addprefix $(CURDIR)/,$(TIDY_FILES))

FORMAT_FILES := $(HEADERS) $(SRCS) $(UNIT_SRCS) $(TSAN_UNIT_SRCS) $(PROPERTY_SRCS) $(FUZZ_SRCS) \
	$(VENDOR_THEFT_DIR)/theft.h $(VENDOR_THEFT_DIR)/theft.c tests/support/fff.h

CRITERION_CFLAGS := $(shell pkg-config --cflags criterion 2>/dev/null)
CRITERION_LIBS := $(shell pkg-config --libs criterion 2>/dev/null)
ifeq ($(strip $(CRITERION_LIBS)),)
CRITERION_LIBS := -lcriterion
endif

FUZZ_CFLAGS := -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
FUZZ_ARGS ?= $(FUZZ_DIR)/corpus -max_total_time=10

LCOV_INFO := $(BUILD_DIR)/coverage/enki.info
LCOV_FILTERED_INFO := $(BUILD_DIR)/coverage/enki.filtered.info
COVERAGE_HTML_DIR := $(BUILD_DIR)/html
LCOV_IGNORE_ERRORS ?= --ignore-errors inconsistent,inconsistent,mismatch,mismatch,gcov,gcov,unused,unused

.PHONY: all lib install test test-binaries test-unit test-property fuzz fuzz-bin coverage tidy \
	format format-check compile-commands compile-commands-fallback compiler-detection clean distclean

all: lib

lib: $(LIB)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(CFLAGS_ALL) -c $< -o $@

$(BUILD_DIR)/tests/unit/%_tsan: tests/unit/%_tsan.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(CFLAGS_ALL) $< $(LIB_OBJS) $(LDFLAGS_ALL) -o $@

$(BUILD_DIR)/tests/unit/%: tests/unit/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(CRITERION_CFLAGS) $(CFLAGS_ALL) $< $(LIB_OBJS) \
		$(LDFLAGS_ALL) $(CRITERION_LIBS) -o $@

$(BUILD_DIR)/tests/property/%: tests/property/%.c $(LIB_OBJS) $(THEFT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(CFLAGS_ALL) $< $(LIB_OBJS) $(THEFT_OBJS) \
		$(LDFLAGS_ALL) -o $@

$(BUILD_DIR)/tests/fuzz/%: tests/fuzz/%.c $(LIB_OBJS)
	@if ! $(CC) --version 2>/dev/null | grep -qi clang; then \
		echo "libFuzzer target requires Clang; use CC=clang"; \
		exit 2; \
	fi
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(BASE_CFLAGS) $(WARN_CFLAGS) $(BUILD_CFLAGS_asan) \
		$(FUZZ_CFLAGS) $< $(LIB_OBJS) $(LDFLAGS_ALL) -o $@

install: lib
	install -d $(PREFIX)/lib $(PREFIX)/include/enki
	install -m 0644 $(LIB) $(PREFIX)/lib/libenki.a
	install -m 0644 $(HEADERS) $(PREFIX)/include/enki/

test-binaries: $(ACTIVE_UNIT_BINS) $(PROPERTY_BINS)

test: test-unit test-property

test-unit: $(ACTIVE_UNIT_BINS)
	@set -eu; for test_bin in $(ACTIVE_UNIT_BINS); do \
		if [ "$(BUILD_TYPE)-$(UNAME_S)" = "tsan-Darwin" ]; then \
			"$$test_bin"; \
		else \
			"$$test_bin" --jobs 1; \
		fi; \
	done

test-property: $(PROPERTY_BINS)
	@set -eu; for test_bin in $(PROPERTY_BINS); do "$$test_bin"; done

fuzz-bin: $(FUZZ_BINS)

fuzz: $(FUZZ_BINS)
	@set -eu; for fuzz_bin in $(FUZZ_BINS); do "$$fuzz_bin" $(FUZZ_ARGS); done

coverage:
	$(MAKE) BUILD_TYPE=coverage test
	@mkdir -p $(BUILD_DIR)/coverage $(COVERAGE_HTML_DIR)
	lcov --capture --directory $(BUILD_DIR) --output-file $(LCOV_INFO) $(LCOV_IGNORE_ERRORS)
	lcov --remove $(LCOV_INFO) '*/tests/*' '*/nix/store/*' --output-file $(LCOV_FILTERED_INFO) \
		$(LCOV_IGNORE_ERRORS)
	genhtml $(LCOV_FILTERED_INFO) --output-directory $(COVERAGE_HTML_DIR)

tidy:
	@test -f compile_commands.json || \
		{ echo "compile_commands.json missing; run 'make compile-commands' first"; exit 2; }
	clang-tidy --quiet -p . $(TIDY_FILES_ABS) --warnings-as-errors='*'

format:
	clang-format -i $(FORMAT_FILES)
	@if command -v treefmt >/dev/null 2>&1; then treefmt; fi

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)
	@if command -v treefmt >/dev/null 2>&1; then treefmt --fail-on-change; fi

compile-commands:
	@if command -v bear >/dev/null 2>&1; then \
		bear --output compile_commands.bear.json -- \
			$(MAKE) BUILD_TYPE=$(BUILD_TYPE) CC=$(CC) clean test-binaries fuzz-bin || true; \
	fi
	$(MAKE) BUILD_TYPE=$(BUILD_TYPE) CC=$(CC) compile-commands-fallback

compile-commands-fallback:
	@printf '[\n' > compile_commands.json
	@first=1; \
	for src in $(TIDY_FILES); do \
		flags="$(CPPFLAGS_ALL) $(CFLAGS_ALL)"; \
		case "$$src" in \
			tests/unit/*) flags="$(CPPFLAGS_ALL) $(CRITERION_CFLAGS) $(CFLAGS_ALL)" ;; \
			tests/fuzz/*) flags="$(CPPFLAGS_ALL) $(BASE_CFLAGS) $(WARN_CFLAGS) $(BUILD_CFLAGS_asan) $(FUZZ_CFLAGS)" ;; \
		esac; \
		if [ "$$first" -eq 0 ]; then printf ',\n' >> compile_commands.json; fi; \
		first=0; \
		printf '  {"directory":"%s","command":"%s %s -c %s","file":"%s"}' \
			'$(CURDIR)' '$(CC)' "$$flags" '$(CURDIR)'/"$$src" '$(CURDIR)'/"$$src" \
			>> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json

compiler-detection:
	@printf 'CC=%s\n' '$(CC)'
	@printf 'GNU_OR_CLANG_WARNINGS=%s\n' '$(if $(WARN_CFLAGS),yes,no)'
	@printf 'BUILD_TYPE=%s\n' '$(BUILD_TYPE)'
	@printf 'CFLAGS=%s\n' '$(CFLAGS_ALL)'

clean:
	rm -rf $(BUILD_DIR)

distclean:
	rm -rf build compile_commands.json result result-*

-include $(LIB_OBJS:.o=.d)
-include $(THEFT_OBJS:.o=.d)
