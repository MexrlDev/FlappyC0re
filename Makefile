# SPDX-License-Identifier: MIT
CC      := gcc
OBJCOPY := objcopy
PY      := python3

CFLAGS := -m64 -ffreestanding -nostdinc -fPIE \
          -fno-stack-protector -fno-builtin \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -mno-red-zone -fno-omit-frame-pointer \
          -Os -Wall -Wextra -Wno-unused-parameter \
          -I src

LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -pie \
           -Wl,-T,$(CURDIR)/linker.ld \
           -Wl,--build-id=none

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=build/%.o)
ASM_OBJ := build/assets.o

TARGET_ELF := flappy.elf
TARGET_BIN := flappy.bin
TARGET_HEX := flappy.hex

ASSET_OUTPUTS := src/assets.h src/assets.bin src/assets.S
ASSET_SCRIPT  := tools/bake_assets.py

.PHONY: all clean hex assets

all: $(TARGET_BIN) $(TARGET_ELF) $(TARGET_HEX)

ifeq ($(wildcard $(ASSET_SCRIPT)),)
$(error $(ASSET_SCRIPT) not found.  Check with: git ls-files tools/)
endif

src/.assets.stamp: $(ASSET_SCRIPT) \
                   $(wildcard assets/*.png) $(wildcard assets/*.wav)
	@mkdir -p src
	$(PY) $(ASSET_SCRIPT)
	@touch $@

$(ASSET_OUTPUTS): src/.assets.stamp

assets: $(ASSET_OUTPUTS)

build:
	@mkdir -p build

build/%.o: src/%.c $(ASSET_OUTPUTS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/assets.o: src/assets.S $(ASSET_OUTPUTS) | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET_ELF): $(OBJ) $(ASM_OBJ) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(ASM_OBJ)

$(TARGET_BIN): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@
	@printf "  elf: %8d bytes\n" $$(stat -c%s $(TARGET_ELF) 2>/dev/null || stat -f%z $(TARGET_ELF))
	@printf "  bin: %8d bytes\n" $$(stat -c%s $(TARGET_BIN) 2>/dev/null || stat -f%z $(TARGET_BIN))

$(TARGET_HEX): $(TARGET_BIN)
	@xxd -p $(TARGET_BIN) | tr -d '\n' > $@
	@printf "  hex: %8d bytes\n" $$(stat -c%s $(TARGET_HEX) 2>/dev/null || stat -f%z $(TARGET_HEX))

hex: $(TARGET_HEX)

clean:
	rm -rf build
	rm -f $(TARGET_ELF) $(TARGET_BIN) $(TARGET_HEX)
	rm -f $(ASSET_OUTPUTS) src/.assets.stamp
