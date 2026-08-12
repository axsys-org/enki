ROOT := ..
BUILD := build
TARGET := $(BUILD)/enki-os.elf

CC = clang
LD = ld.lld
PYTHON ?= python3
QEMU ?= qemu-system-x86_64
LLVM_READELF ?= llvm-readelf
LLVM_NM ?= llvm-nm

CLANG_RESOURCE := $(shell $(CC) -print-resource-dir)
CPPFLAGS := -Iinclude -I$(ROOT)/pkg/axsys/include -I$(ROOT)/pkg/plan/include \
	-I$(ROOT)/pkg/plan/src -I$(ROOT)/pkg/enki/include \
	-isystem $(CLANG_RESOURCE)/include \
	-D_GNU_SOURCE -D_Thread_local= -DPL_STORE_REGION_BYTES='(((size_t)1)<<27)'
CFLAGS := --target=x86_64-none-elf -std=c23 -O2 -g -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel \
	-mgeneral-regs-only -Wall -Wextra -Wno-unused-function \
	-Wno-unused-parameter -Wno-missing-field-initializers
ASFLAGS := --target=x86_64-none-elf -g -ffreestanding -fno-pic -fno-pie \
	-mno-red-zone -mcmodel=kernel
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	-T linker.ld

AXSYS_SRCS := \
	$(ROOT)/pkg/axsys/src/base58.c \
	$(ROOT)/pkg/axsys/src/sb.c \
	$(ROOT)/pkg/axsys/src/stb_ds.c \
	$(ROOT)/pkg/axsys/src/vector.c

PLAN_SRCS := \
	$(ROOT)/pkg/plan/src/build.c \
	$(ROOT)/pkg/plan/src/bytecode.c \
	$(ROOT)/pkg/plan/src/canon.c \
	$(ROOT)/pkg/plan/src/debug.c \
	$(ROOT)/pkg/plan/src/eval.c \
	$(ROOT)/pkg/plan/src/heap.c \
	$(ROOT)/pkg/plan/src/nat.c \
	$(ROOT)/pkg/plan/src/op.c \
	$(ROOT)/pkg/plan/src/pin.c \
	$(ROOT)/pkg/plan/src/silo.c \
	$(ROOT)/pkg/plan/src/store.c \
	$(ROOT)/pkg/plan/src/xtract.c

ENKI_SRCS := $(ROOT)/pkg/enki/src/wisp.c

OS_C_SRCS := \
	src/arena.c src/boot.c src/gmp.c src/kernel.c src/libc.c src/loader.c \
	src/lmdb.c src/profile.c src/pthread.c src/rom.c src/rplan.c \
	src/scheduler.c src/sha256.c src/silo_backend.c src/store_wrap.c
OS_S_SRCS := src/entry.S src/interrupts.S src/setjmp.S $(BUILD)/rom_data.S
ROM_INPUTS := $(shell find $(ROOT)/reaver/src -type f -print 2>/dev/null)

ALL_C_SRCS := $(AXSYS_SRCS) $(PLAN_SRCS) $(ENKI_SRCS) $(OS_C_SRCS) $(BUILD)/rom_index.c
ALL_S_SRCS := $(OS_S_SRCS)

define objname
$(BUILD)/obj/$(subst /,_,$(subst ../,up_,$(basename $(1)))).o
endef

OBJS := $(foreach s,$(ALL_C_SRCS),$(call objname,$(s))) \
	$(foreach s,$(ALL_S_SRCS),$(call objname,$(s)))
DEPS := $(OBJS:.o=.d)

.PHONY: all run check test test-self clean check-reaver

all: $(TARGET)

check-reaver:
	@test -d $(ROOT)/reaver/src || { \
		echo 'enki-os: reaver/src is missing; initialize the reaver submodule' >&2; \
		exit 1; \
	}

$(BUILD)/rom_data.S $(BUILD)/rom_index.c &: tools/mkrom.py $(ROM_INPUTS) | check-reaver
	@mkdir -p $(BUILD)
	$(PYTHON) tools/mkrom.py $(ROOT)/reaver/src $(BUILD)/rom_data.S $(BUILD)/rom_index.c

$(TARGET): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

define compile_c
$(call objname,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) -MMD -MP -c $$< -o $$@
endef

$(foreach s,$(ALL_C_SRCS),$(eval $(call compile_c,$(s))))

# The store creates a 512 MiB-per-space compiler heap on the hosted build.
# Keep its source unchanged and clamp only that translation unit through a
# symbol interposition used by enki-os.
$(call objname,$(ROOT)/pkg/plan/src/store.c): CFLAGS += -Dpl_heap_new=os_pl_heap_new

define compile_s
$(call objname,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CPPFLAGS) $$(ASFLAGS) -MMD -MP -c $$< -o $$@
endef

$(foreach s,$(ALL_S_SRCS),$(eval $(call compile_s,$(s))))

run: $(TARGET)
	$(QEMU) -machine q35 -cpu max -smp 1 -m 1024M -kernel $(TARGET) \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04

check: $(TARGET)
	$(LLVM_READELF) -h $(TARGET) | grep -q 'Machine:.*Advanced Micro Devices X86-64'
	$(LLVM_READELF) -l $(TARGET) | grep -q 'LOAD'
	@test -z "$$($(LLVM_NM) -u $(TARGET))"
	@! $(LLVM_READELF) -d $(TARGET) | grep -q '(NEEDED)'
	@$(PYTHON) -c 'p=open("$(TARGET)","rb").read(32768); assert bytes.fromhex("d65052e8") in p, "Multiboot2 header is not in the first 32 KiB"'

test-self: $(TARGET)
	$(PYTHON) tests/qemu_test.py --qemu $(QEMU) --kernel $(TARGET) --selftest

test: check test-self
	$(PYTHON) tests/qemu_test.py --qemu $(QEMU) --kernel $(TARGET) --integration

clean:
	rm -rf $(BUILD)

-include $(DEPS)
