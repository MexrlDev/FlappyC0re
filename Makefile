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

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)
ASM := src/assets.S
ASM_OBJ := build/assets.o

TARGET_ELF := flappy.elf
TARGET_BIN := flappy.bin
TARGET_HEX := flappy.hex

.PHONY: all clean hex assets

all: $(TARGET_BIN) $(TARGET_ELF) $(TARGET_HEX)

assets: src/assets.bin

src/assets.bin: tools/bake_assets.py assets/*.png assets/*.wav
	@mkdir -p src
	$(PY) tools/bake_assets.py

build/%.o: src/%.c src/assets.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(ASM_OBJ): src/assets.S src/assets.bin | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	@mkdir -p build

$(TARGET_ELF): $(OBJ) $(ASM_OBJ) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJ) $(ASM_OBJ)

$(TARGET_BIN): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@
	@printf "  elf: %8d bytes\n" `stat -c%s $(TARGET_ELF)`
	@printf "  bin: %8d bytes\n" `stat -c%s $(TARGET_BIN)`

hex: $(TARGET_BIN)
	@xxd -p $(TARGET_BIN) | tr -d '\n' > $(TARGET_HEX)
	@printf "  hex: %8d bytes\n" `stat -c%s $(TARGET_HEX)`

clean:
	rm -rf build
	rm -f $(TARGET_ELF) $(TARGET_BIN) $(TARGET_HEX)
	rm -f src/assets.bin src/assets.h src/assets.S
