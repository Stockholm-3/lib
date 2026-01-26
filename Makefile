SHELL := bash

# ------------------------------------------------------------
# Compiler + global settings
# ------------------------------------------------------------
CC          := gcc

BUILD_MODE  ?= debug
BUILD_DIR   := build/$(BUILD_MODE)

ifeq ($(BUILD_MODE),release)
    CFLAGS_BASE := -O3 -DNDEBUG
    BUILD_TYPE  := Release
else
    CFLAGS_BASE := -O1 -g
    BUILD_TYPE  := Debug
endif

# ------------------------------------------------------------
# Include directories (RECURSIVE)
# ------------------------------------------------------------
LIB_INCLUDES := $(shell find . -type d)
INCLUDES     := $(addprefix -I,$(LIB_INCLUDES))

# ------------------------------------------------------------
# Compiler flags
# ------------------------------------------------------------
CFLAGS := $(CFLAGS_BASE) -MMD -MP $(INCLUDES)

# ------------------------------------------------------------
# Source and object files
# ------------------------------------------------------------
LIB_FILES := $(shell find . -type f -name '*.c')

OBJ_FILES := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LIB_FILES))
DEP_FILES := $(OBJ_FILES:.o=.d)

# ------------------------------------------------------------
# Default target: compile all .c files
# ------------------------------------------------------------
.PHONY: all
all: $(OBJ_FILES)
	@echo "Library compilation complete. [$(BUILD_TYPE)]"

# ------------------------------------------------------------
# Compile rule (NO LINKING)
# ------------------------------------------------------------
$(BUILD_DIR)/%.o: %.c
	@echo "Compiling $< ... [$(BUILD_TYPE)]"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------
# Utilities
# ------------------------------------------------------------
.PHONY: clean
clean:
	@rm -rf build
	@echo "Cleaned build artifacts."

# Auto-include dependency files
-include $(DEP_FILES)
