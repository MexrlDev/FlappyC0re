NAME    := flappy
CC      := gcc
OBJCOPY := objcopy
PYTHON  ?= python3

SRC_DIR := src
BUILD   := build

CFLAGS := -m64 -ffreestanding -nostdinc -fPIE \
          -fno-stack-protector -fno-builtin \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -mno-red-zone -fno-omit-frame-pointer \
          -Os -Wall -Wextra -Wno-unused-parameter \
          -I$(SRC_DIR)

LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -pie \
           -Wl,-T,$(CURDIR)/linker.ld -Wl,--build-id=none

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean hex assets font

all: $(NAME).bin

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(NAME).elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(NAME).bin: $(NAME).elf
	$(OBJCOPY) -O binary $< $@
	@printf 'Built %s (%s bytes)\n' "$@" "$$(stat -c%s $@ 2>/dev/null || stat -f%z $@)"

hex: $(NAME).bin
	xxd -p $< | tr -d '\n' > $(NAME).hex
	@printf 'Wrote %s (%s bytes)\n' "$(NAME).hex" "$$(stat -c%s $(NAME).hex 2>/dev/null || stat -f%z $(NAME).hex)"

assets:
	$(PYTHON) tools/bake_assets.py

font:
	$(PYTHON) tools/bake_font.py

clean:
	rm -rf $(BUILD)
	rm -f $(NAME).elf $(NAME).bin $(NAME).hex
