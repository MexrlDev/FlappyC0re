# SPDX-License-Identifier: MIT
CC     := gcc
LD     := ld
OBJCOPY:= objcopy
PY     := python3

CFLAGS := -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -fno-pic -fno-pie -mno-red-zone -mcmodel=large \
          -Os -Wall -Wextra -Wno-unused-parameter \
          -I src

LDFLAGS := -T linker.ld -nostdlib -static -no-pie

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=build/%.o)
ASM_OBJ := build/assets.o

TARGET_ELF := flappy.elf
TARGET_BIN := flappy.bin
TARGET_HEX := flappy.hex

# All files the bake script produces
ASSET_OUTPUTS := src/assets.h src/assets.bin src/assets.S

.PHONY: all clean hex

all: $(TARGET_BIN) $(TARGET_ELF) $(TARGET_HEX)

# Grouped target: one recipe run produces all three files.
# Requires GNU make >= 4.3 (Ubuntu 24.04 ships 4.3).
$(ASSET_OUTPUTS) &: tools/bake_assets.py $(wildcard assets/*.png assets/*.wav)
	@mkdir -p src
	$(PY) tools/bake_assets.py

build:
	@mkdir -p build

# Pattern rule for C sources.  assets.h is a generated prerequisite.
build/%.o: src/%.c src/assets.h | build
	$(CC) $(CFLAGS) -c $< -o $@

# Assembly blob that .incbin's src/assets.bin
build/assets.o: src/assets.S src/assets.bin | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET_ELF): $(OBJ) $(ASM_OBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJ) $(ASM_OBJ)

$(TARGET_BIN): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@
	@printf "  elf: %8d bytes\n" $$(stat -c%s $(TARGET_ELF) 2>/dev/null || stat -f%z $(TARGET_ELF))
	@printf "  bin: %8d bytes\n" $$(stat -c%s $(TARGET_BIN) 2>/dev/null || stat -f%z $(TARGET_BIN))

hex: $(TARGET_BIN)
	@xxd -p $(TARGET_BIN) | tr -d '\n' > $(TARGET_HEX)
	@printf "  hex: %8d bytes\n" $$(stat -c%s $(TARGET_HEX) 2>/dev/null || stat -f%z $(TARGET_HEX))

clean:
	rm -rf build
	rm -f $(TARGET_ELF) $(TARGET_BIN) $(TARGET_HEX)
	rm -f $(ASSET_OUTPUTS)
