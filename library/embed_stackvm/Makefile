CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ROOT_DIR := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/embed_stackvm
STACKVM_DIR := ../stackvm
STACKVM_LIB := $(ROOT_DIR)/build/stackvm/libstackvm.a
TARGET := $(BUILD_DIR)/embed-stackvm
TESTS := $(BUILD_DIR)/embed-stackvm-tests

ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN := powershell -NoProfile -Command "Remove-Item -LiteralPath '$(BUILD_DIR_WIN)' -Recurse -Force -ErrorAction SilentlyContinue"
else
MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all stackvm-lib check clean

all: $(TARGET) $(TESTS)

stackvm-lib:
	$(MAKE) -C $(STACKVM_DIR) lib

$(TARGET): main.cpp static_heap_allocator.hpp stackvm-lib | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(STACKVM_DIR) main.cpp $(STACKVM_LIB) -o $@

$(TESTS): test_embed_stackvm.cpp static_heap_allocator.hpp \
          embed_pool.h embed_pool.c embed_pool_allocator.hpp stackvm-lib | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(STACKVM_DIR) -I. test_embed_stackvm.cpp embed_pool.c $(STACKVM_LIB) -o $@

$(BUILD_DIR):
	$(MKDIR)

check: all
	$(TESTS)
	$(TARGET)

clean:
	$(CLEAN)
