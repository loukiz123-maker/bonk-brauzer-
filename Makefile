# Bonk Brauzer — build orchestration.
#
# The canonical build is dependency-free: any C++20 compiler + GNU make.
# CMakeLists.txt mirrors this file for IDE/CI users (see `cmake` job in CI).
#
#   make                  # build build/bonk and build/bonk_tests (debug, -O1 -g)
#   make test             # build + run the unit tests
#   make run ARGS="modules --stage 1"
#   make MODE=release     # -O2 -DNDEBUG
#   make SAN=address,undefined test   # ASan + UBSan build in build-address,undefined
#   make docs             # regenerate docs/modules.generated.md
#   make verify           # test + selftest + docs-in-sync (what CI runs)

CXX       ?= g++
CXXSTD    ?= c++20
BUILD_DIR ?= build
MODE      ?= debug
SAN       ?= none
WARN      ?= -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-qual

ifeq ($(MODE),release)
  MODE_FLAGS := -O2 -DNDEBUG
else
  MODE_FLAGS := -O1 -g
endif

ifneq ($(SAN),none)
  SAN_FLAGS := -fsanitize=$(SAN) -fno-omit-frame-pointer -fno-sanitize-recover=all -g
  BUILD_DIR := $(BUILD_DIR)-$(SAN)
endif

CPPFLAGS  := -Iinclude -Itests
CXXFLAGS  := -std=$(CXXSTD) $(WARN) $(MODE_FLAGS) $(SAN_FLAGS) -MMD -MP
LDFLAGS   += $(SAN_FLAGS)

BIN        := $(BUILD_DIR)/bonk
TESTS_BIN  := $(BUILD_DIR)/bonk_tests

LIB_SRC    := $(filter-out src/apps/%,$(wildcard src/*/*.cpp))
APP_SRC    := $(wildcard src/apps/*.cpp)
TEST_SRC   := $(wildcard tests/*.cpp) $(wildcard tests/*/*.cpp)

LIB_OBJ    := $(LIB_SRC:%.cpp=$(BUILD_DIR)/obj/%.o)
APP_OBJ    := $(APP_SRC:%.cpp=$(BUILD_DIR)/obj/%.o)
TEST_OBJ   := $(TEST_SRC:%.cpp=$(BUILD_DIR)/obj/%.o)
DEP        := $(LIB_OBJ:.o=.d) $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

.PHONY: all test verify run docs docs-check selftest clean info help

all: $(BIN) $(TESTS_BIN)

$(BIN): $(LIB_OBJ) $(APP_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TESTS_BIN): $(LIB_OBJ) $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: $(TESTS_BIN)
	./$(TESTS_BIN) $(TEST_ARGS)

selftest: $(BIN)
	./$(BIN) selftest

run: $(BIN)
	./$(BIN) $(ARGS)

docs: $(BIN)
	./$(BIN) modules --markdown > docs/modules.generated.md
	@echo "docs/modules.generated.md updated"

docs-check: $(BIN)
	@./$(BIN) modules --markdown > $(BUILD_DIR)/modules.generated.md
	@diff -u docs/modules.generated.md $(BUILD_DIR)/modules.generated.md || \
	  (echo "docs/modules.generated.md is stale: run 'make docs'"; exit 1)

verify: test selftest docs-check
	@echo "verify: OK"

info:
	@echo "CXX       = $(CXX)"
	@echo "CXXSTD    = $(CXXSTD)"
	@echo "MODE      = $(MODE) ($(MODE_FLAGS))"
	@echo "SAN       = $(SAN)"
	@echo "BUILD_DIR = $(BUILD_DIR)"
	@echo "sources   = $(words $(LIB_SRC)) library, $(words $(APP_SRC)) app, $(words $(TEST_SRC)) test"
	@$(CXX) --version | head -1

clean:
	rm -rf build build-* 

help:
	@sed -n 's/^#   //p' Makefile | head -12

-include $(DEP)
