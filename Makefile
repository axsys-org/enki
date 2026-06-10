BUILD_TYPE ?= debug
PROFILE ?=
CC ?= cc
PREFIX ?= /usr/local
BUILD_DIR ?= build/$(BUILD_TYPE)
AR ?= ar

VALID_BUILD_TYPES := debug release asan ubsan tsan coverage profile

# Per-package include paths enforce the layering (axsys < plan < enki):
# compiling pkg/plan, the pkg/enki include directory is not on the path.
AXSYS_INC := -Ipkg/axsys/include
PLAN_INC := -Ipkg/plan/include $(AXSYS_INC)
ENKI_INC := -Ipkg/enki/include $(PLAN_INC)

BASE_CPPFLAGS := -Itests/support -Itests/property/vendor/theft $(NIX_CFLAGS_COMPILE)
BASE_CFLAGS := -std=c23 -MMD -MP -D_GNU_SOURCE

WARN_COMMON := -Wall -Wextra  \
	-Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
	-Wmissing-prototypes -Wold-style-definition -Wnull-dereference \
	-Wdouble-promotion -Werror \
	-Wno-sign-conversion -Wno-char-subscripts -Wno-unused-function -Wno-gnu-label-as-value

WARN_CFLAGS = $(WARN_COMMON)

HARDEN_CFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong

BUILD_CFLAGS_debug := -O0 -g3 -DDEBUG
BUILD_CFLAGS_release := -O3 -DNDEBUG $(HARDEN_CFLAGS)
BUILD_CFLAGS_profile := $(BUILD_CFLAGS_release) -g3 -fno-omit-frame-pointer -fdebug-info-for-profiling -fno-inline
BUILD_CFLAGS_asan := -O1 -g3 -fsanitize=address -fno-omit-frame-pointer $(HARDEN_CFLAGS)
BUILD_CFLAGS_ubsan := -O1 -g3 -fsanitize=undefined -fno-omit-frame-pointer $(HARDEN_CFLAGS)
BUILD_CFLAGS_tsan := -O1 -g3 -fsanitize=thread -fno-omit-frame-pointer $(HARDEN_CFLAGS)
BUILD_CFLAGS_coverage := -O1 -g3 --coverage -Wno-pedantic $(HARDEN_CFLAGS)

BUILD_LDFLAGS_debug :=
BUILD_LDFLAGS_release :=
BUILD_LDFLAGS_profile :=
BUILD_LDFLAGS_asan := -fsanitize=address
BUILD_LDFLAGS_ubsan := -fsanitize=undefined
BUILD_LDFLAGS_tsan := -fsanitize=thread
BUILD_LDFLAGS_coverage := --coverage

ifeq ($(filter $(BUILD_TYPE),$(VALID_BUILD_TYPES)),)
$(error BUILD_TYPE must be one of $(VALID_BUILD_TYPES))
endif

# GC stress: every reserve collects (see pkg/plan/include/plan/heap.h)
ifdef GC_STRESS
BASE_CFLAGS += -DPL_GC_STRESS
endif

APP_DIR := pkg/enki/app
APP_SRCS := $(wildcard $(APP_DIR)/*.c)
APP_BINS := $(patsubst $(APP_DIR)/%.c,$(BUILD_DIR)/bin/%,$(APP_SRCS))

CPPFLAGS_ALL := $(BASE_CPPFLAGS) $(CPPFLAGS)
CFLAGS_ALL := $(BASE_CFLAGS) $(WARN_CFLAGS) $(BUILD_CFLAGS_$(BUILD_TYPE)) $(CFLAGS)
LDFLAGS_ALL := $(BUILD_LDFLAGS_$(BUILD_TYPE)) $(LDFLAGS) -L/opt/homebrew/lib -lgmp -llmdb -lcrypto

ifeq ($(PROFILE),tracy)
CPPFLAGS_ALL += -I/opt/homebrew/opt/tracy/include/tracy
CFLAGS_ALL += -DTRACY_ENABLE
LDFLAGS_ALL += -L/opt/homebrew/opt/tracy/lib -Wl,-rpath,/opt/homebrew/opt/tracy/lib -lTracyClient
endif

AXSYS_SRCS := $(wildcard pkg/axsys/src/*.c)
PLAN_SRCS := $(wildcard pkg/plan/src/*.c)
ENKI_SRCS := $(wildcard pkg/enki/src/*.c)
HEADERS := $(wildcard pkg/axsys/include/axsys/*.h) \
	$(wildcard pkg/plan/include/plan/*.h) \
	$(wildcard pkg/enki/include/enki/*.h)

UNIT_DIR := tests/unit
PROPERTY_DIR := tests/property
FUZZ_DIR := tests/fuzz
PERF_DIR := tests/perf
VENDOR_THEFT_DIR := tests/property/vendor/theft

TSAN_UNIT_SRCS := $(wildcard $(UNIT_DIR)/*_tsan.c)
UNIT_SRCS := $(filter-out $(TSAN_UNIT_SRCS),$(wildcard $(UNIT_DIR)/*.c))
PROPERTY_SRCS := $(wildcard $(PROPERTY_DIR)/*.c)
THEFT_SRCS := $(wildcard $(VENDOR_THEFT_DIR)/*.c)
FUZZ_SRCS := $(wildcard $(FUZZ_DIR)/*.c)
PERF_SRCS := $(wildcard $(PERF_DIR)/*.c)

AXSYS_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(AXSYS_SRCS))
PLAN_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(PLAN_SRCS))
ENKI_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(ENKI_SRCS))
LIB_OBJS := $(AXSYS_OBJS) $(PLAN_OBJS) $(ENKI_OBJS)

UNIT_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(UNIT_SRCS))
TSAN_UNIT_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(TSAN_UNIT_SRCS))
PROPERTY_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(PROPERTY_SRCS))
THEFT_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(THEFT_SRCS))
FUZZ_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(FUZZ_SRCS))
PERF_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(PERF_SRCS))

LIB_AXSYS := $(BUILD_DIR)/lib/libaxsys.a
LIB_PLAN := $(BUILD_DIR)/lib/libplan.a
LIB_ENKI := $(BUILD_DIR)/lib/libenki.a
LIBS := $(LIB_AXSYS) $(LIB_PLAN) $(LIB_ENKI)

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(BUILD_TYPE)-$(UNAME_S),tsan-Darwin)
ACTIVE_UNIT_BINS := $(TSAN_UNIT_BINS)
else
ACTIVE_UNIT_BINS := $(UNIT_BINS)
endif

TIDY_FILES := $(AXSYS_SRCS) $(PLAN_SRCS) $(ENKI_SRCS) $(UNIT_SRCS) $(PROPERTY_SRCS) $(FUZZ_SRCS) $(TSAN_UNIT_SRCS)
TIDY_FILES_ABS := $(addprefix $(CURDIR)/,$(TIDY_FILES))

FORMAT_FILES := $(HEADERS) $(AXSYS_SRCS) $(PLAN_SRCS) $(ENKI_SRCS) $(APP_SRCS) \
	$(UNIT_SRCS) $(TSAN_UNIT_SRCS) $(PROPERTY_SRCS) $(FUZZ_SRCS) \
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

.PHONY: all lib bin install test test-binaries test-unit test-property fuzz fuzz-bin perf-binaries coverage tidy \
	check-layering format format-check compile-commands clean distclean

all: lib bin

bin: $(APP_BINS)

$(BUILD_DIR)/bin/%: $(APP_DIR)/%.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CFLAGS_ALL) $< $(LIB_ENKI) $(LIB_PLAN) $(LIB_AXSYS) $(LDFLAGS_ALL) -o $@

lib: $(LIBS)

$(LIB_AXSYS): $(AXSYS_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(LIB_PLAN): $(PLAN_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(LIB_ENKI): $(ENKI_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# Layered compile rules (R1-R3): each package sees only its own include
# path and the layers beneath it.
$(BUILD_DIR)/pkg/axsys/%.o: pkg/axsys/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(AXSYS_INC) $(CFLAGS_ALL) -c $< -o $@

$(BUILD_DIR)/pkg/plan/%.o: pkg/plan/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(PLAN_INC) -Ipkg/plan/src $(CFLAGS_ALL) -c $< -o $@

$(BUILD_DIR)/pkg/enki/%.o: pkg/enki/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CFLAGS_ALL) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CFLAGS_ALL) -c $< -o $@

$(BUILD_DIR)/tests/unit/%_tsan: tests/unit/%_tsan.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CFLAGS_ALL) $< $(LIB_ENKI) $(LIB_PLAN) $(LIB_AXSYS) $(LDFLAGS_ALL) -o $@

$(BUILD_DIR)/tests/unit/%: tests/unit/%.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CRITERION_CFLAGS) $(CFLAGS_ALL) $< $(LIB_ENKI) $(LIB_PLAN) $(LIB_AXSYS) \
		$(LDFLAGS_ALL) $(CRITERION_LIBS) -o $@

$(BUILD_DIR)/tests/property/%: tests/property/%.c $(LIBS) $(THEFT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CFLAGS_ALL) $< $(LIB_ENKI) $(LIB_PLAN) $(LIB_AXSYS) $(THEFT_OBJS) \
		$(LDFLAGS_ALL) -o $@

$(BUILD_DIR)/tests/fuzz/%: tests/fuzz/%.c $(LIBS)
	@if ! $(CC) --version 2>/dev/null | grep -qi clang; then \
		echo "libFuzzer target requires Clang; use CC=clang"; \
		exit 2; \
	fi
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(BASE_CFLAGS) $(WARN_CFLAGS) $(BUILD_CFLAGS_asan) \
		$(FUZZ_CFLAGS) $< $(LIB_ENKI) $(LIB_PLAN) $(LIB_AXSYS) $(LDFLAGS_ALL) -o $@

$(BUILD_DIR)/tests/perf/%: tests/perf/%.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_ALL) $(ENKI_INC) $(CFLAGS_ALL) $< $(LIB_ENKI) $(LIB_PLAN) $(LIB_AXSYS) $(LDFLAGS_ALL) -o $@

install: lib bin
	install -d $(PREFIX)/lib $(PREFIX)/include/axsys $(PREFIX)/include/plan $(PREFIX)/include/enki
	install -m 0644 $(LIB_AXSYS) $(PREFIX)/lib/libaxsys.a
	install -m 0644 $(LIB_PLAN) $(PREFIX)/lib/libplan.a
	install -m 0644 $(LIB_ENKI) $(PREFIX)/lib/libenki.a
	install -m 0644 pkg/axsys/include/axsys/*.h $(PREFIX)/include/axsys/
	install -m 0644 pkg/plan/include/plan/*.h $(PREFIX)/include/plan/
	install -m 0644 pkg/enki/include/enki/*.h $(PREFIX)/include/enki/

test-binaries: $(ACTIVE_UNIT_BINS) $(PROPERTY_BINS)

test: check-layering test-unit test-property

test-unit: $(ACTIVE_UNIT_BINS) $(APP_BINS)
	@set -eu; for test_bin in $(ACTIVE_UNIT_BINS); do \
		if [ "$(BUILD_TYPE)-$(UNAME_S)" = "tsan-Darwin" ]; then \
			ENKI_WISP_BIN=$(CURDIR)/$(BUILD_DIR)/bin/wisp "$$test_bin"; \
		else \
			ENKI_WISP_BIN=$(CURDIR)/$(BUILD_DIR)/bin/wisp "$$test_bin" --jobs 1; \
		fi; \
	done

test-property: $(PROPERTY_BINS)
	@set -eu; for test_bin in $(PROPERTY_BINS); do "$$test_bin"; done

# Layering check (R1-R3): grep #include lines against the dependency
# matrix.  axsys must not include plan/ or enki/; plan must not include
# enki/.
check-layering:
	@status=0; \
	if grep -rnE '#include\s+["<](plan|enki)/' pkg/axsys --include='*.c' --include='*.h' 2>/dev/null; then \
		echo "layering violation: pkg/axsys includes plan/ or enki/ headers"; status=1; \
	fi; \
	if grep -rnE '#include\s+["<]enki/' pkg/plan --include='*.c' --include='*.h' 2>/dev/null; then \
		echo "layering violation: pkg/plan includes enki/ headers"; status=1; \
	fi; \
	if [ $$status -eq 0 ]; then echo "check-layering: OK"; fi; \
	exit $$status

fuzz-bin: $(FUZZ_BINS)

fuzz: $(FUZZ_BINS)
	@set -eu; for fuzz_bin in $(FUZZ_BINS); do "$$fuzz_bin" $(FUZZ_ARGS); done

perf-binaries: $(PERF_BINS)

coverage:
	$(MAKE) BUILD_TYPE=coverage test
	@mkdir -p $(BUILD_DIR)/coverage $(COVERAGE_HTML_DIR)
	lcov --capture --directory $(BUILD_DIR) --output-file $(LCOV_INFO) $(LCOV_IGNORE_ERRORS)
	lcov --remove $(LCOV_INFO) '*/tests/*' '*/nix/store/*' --output-file $(LCOV_FILTERED_INFO) \
		$(LCOV_IGNORE_ERRORS)
	genhtml $(LCOV_FILTERED_INFO) --output-directory $(COVERAGE_HTML_DIR)

tidy:
	@test -f compile_commands.json || \
		{ echo "compile_commands.json missing; run bear first"; exit 2; }
	clang-tidy --quiet -p . $(TIDY_FILES_ABS) --warnings-as-errors='*'

format:
	clang-format -i $(FORMAT_FILES)
	@if command -v treefmt >/dev/null 2>&1; then treefmt; fi

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)
	@if command -v treefmt >/dev/null 2>&1; then treefmt --fail-on-change; fi

clean:
	rm -rf $(BUILD_DIR)

distclean:
	rm -rf build compile_commands.json result result-*

-include $(LIB_OBJS:.o=.d)
-include $(THEFT_OBJS:.o=.d)
