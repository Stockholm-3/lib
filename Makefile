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
INC_DIR     := includes
LIB_INCLUDES := $(shell find . -type d)
INCLUDES     := $(addprefix -I,$(LIB_INCLUDES)) -I$(INC_DIR)

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

# Show formatting errors without modifying files
.PHONY: format
format:
	@echo "Checking formatting..."
	@unformatted=$$(find . \( -name '*.c' -o -name '*.h' \) -print0 | \
		xargs -0 clang-format -style=file -output-replacements-xml | \
		grep -c "<replacement " || true); \
	if [ $$unformatted -ne 0 ]; then \
		echo "$$unformatted file(s) need formatting"; \
		find . \( -name '*.c' -o -name '*.h' \) -print0 | \
		xargs -0 clang-format -style=file -n -Werror; \
		exit 1; \
	else \
		echo "All files properly formatted"; \
	fi
# Actually fixes formatting
#
.PHONY: format-fix
format-fix:
	@echo "Applying clang-format..."
	find . \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i -style=file
	@echo "Formatting applied."

.PHONY: lint
lint:
	@echo "Running clang-tidy using compile_commands.json..."
	@find just-lib \( -name '*.c' -o -name '*.h' \) ! -path "*/jansson/*" -print0 | \
	while IFS= read -r -d '' file; do \
		echo "→ Linting $$file"; \
		clang-tidy "$$file" \
			--config-file=.clang-tidy \
			--quiet \
			--header-filter='^(src)/' \
			--system-headers=false || true; \
	done
	@echo "Lint complete (see warnings above)."

.PHONY: lint-fix
lint-fix:
	@echo "Running clang-tidy with auto-fix on src/ (excluding jansson)..."
	@find just-lib \( -name '*.c' -o -name '*.h' \) ! -path "*/jansson/*" -print0 | \
	while IFS= read -r -d '' file; do \
		echo "→ Fixing $$file"; \
		clang-tidy "$$file" \
			--config-file=.clang-tidy \
			--fix \
			--fix-errors \
			--header-filter='src/.*\.(h|hpp)$$' \
			--system-headers=false || true; \
	done
	@echo "Auto-fix complete. Please review changes with 'git diff'."

# CI target: fails only on naming violations
.PHONY: lint-ci
lint-ci:
	@echo "Running clang-tidy for CI (naming violations = errors)..."
	@rm -f /tmp/clang-tidy-failed
	@find src \( -name '*.c' -o -name '*.h' \) ! -path "*/jansson/*" -print0 | \
	while IFS= read -r -d '' file; do \
		echo "→ Checking $$file"; \
		if ! clang-tidy "$$file" \
			--config-file=.clang-tidy \
			--quiet \
			--header-filter='^(src)/' \
			--system-headers=false; then \
			touch /tmp/clang-tidy-failed; \
		fi; \
	done
	@if [ -f /tmp/clang-tidy-failed ]; then \
		rm -f /tmp/clang-tidy-failed; \
		echo "❌ Lint failed: naming standard violations found"; \
		exit 1; \
	else \
		echo "✅ Lint passed"; \
	fi
