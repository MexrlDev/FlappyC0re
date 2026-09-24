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
          -I $(SRC_DIR)

LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -pie \
           -Wl,-T,$(CURDIR)/linker.ld -Wl,--build-id=none

# ---- Sources ----
# .S is picked up so the generated assets.S (from bake_assets.py) is
# assembled into the final binary.  If bake_assets.py does not emit a
# .S file, the wildcard simply returns nothing and the build proceeds.
SRCS_C := $(wildcard $(SRC_DIR)/*.c)
SRCS_S := $(wildcard $(SRC_DIR)/*.S)
OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS_C)) \
          $(patsubst $(SRC_DIR)/%.S,$(BUILD)/%.o,$(SRCS_S))

.PHONY: all clean hex assets font

all: $(NAME).bin

$(BUILD):
	@mkdir -p $(BUILD)

# C sources depend on assets.h so a fresh bake triggers a rebuild.
$(BUILD)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/assets.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Assembly (the generated assets.S) goes through gcc too.
$(BUILD)/%.o: $(SRC_DIR)/%.S | $(BUILD)
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
