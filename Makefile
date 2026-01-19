SHELL := bash

# ------------------------------------------------------------
# Compiler + global settings
# ------------------------------------------------------------
CC          := gcc

BUILD_MODE  ?= debug

ifeq ($(BUILD_MODE),release)
    CFLAGS_BASE := -O3 -DNDEBUG
    BUILD_TYPE  := Release
else
    CFLAGS_BASE := -O1 -g
    BUILD_TYPE  := Debug
endif

# ------------------------------------------------------------
# Include directories
# ------------------------------------------------------------
LIB_INCLUDES := $(shell find . -type d)
INCLUDES := $(addprefix -I,$(LIB_INCLUDES))
CFLAGS := $(CFLAGS_BASE) -Wall -Werror -Wfatal-errors $(INCLUDES)

# ------------------------------------------------------------
# Source files
# ------------------------------------------------------------
LIB_FILES := $(shell find . -type f -name '*.c')

OBJ_DIR := build
OBJ_FILES := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LIB_FILES))

# ------------------------------------------------------------
# Default target: compile all .c files
# ------------------------------------------------------------
.PHONY: all
all: $(OBJ_FILES)
	@echo "Compilation complete. [$(BUILD_TYPE)]"

# Compile rule (no linking)
$(OBJ_DIR)/%.o: %.c
	@echo "Compiling $< ... [$(BUILD_TYPE)]"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean
.PHONY: clean
clean:
	@rm -rf build
	@echo "Cleaned build artifacts."
