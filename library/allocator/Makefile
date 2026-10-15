.DEFAULT_GOAL := all
CXX = g++
CC = gcc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
ROOT := $(abspath ../..)
BUILD ?= $(ROOT)/build/allocator
override BUILD := $(abspath $(BUILD))
ifeq ($(filter $(ROOT)/build/%,$(BUILD)),)
$(error BUILD 必须位于仓库根 build/ 下)
endif
EXE := $(if $(or $(ComSpec),$(COMSPEC),$(findstring mingw,$(shell $(CXX) -dumpmachine))),.exe,)
HEADERS := pool.h pool.c containerAllocator.hpp
DEMO := $(BUILD)/allocatorDemo$(EXE)
TESTS := $(BUILD)/poolLinked$(EXE) $(BUILD)/poolLinear$(EXE) $(BUILD)/containersLinked$(EXE) $(BUILD)/containersLinear$(EXE)
.PHONY: all check clean
all: $(DEMO) $(TESTS)
$(BUILD):
	mkdir -p "$@"
$(DEMO): main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@
$(BUILD)/poolLinked$(EXE): testPool.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@
$(BUILD)/poolLinear$(EXE): testPool.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -DMEMORY_ALLOCATOR_TYPE=MEMORY_ALLOCATOR_LINEAR $< -o $@
$(BUILD)/containersLinked$(EXE): testAllocator.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@
$(BUILD)/containersLinear$(EXE): testAllocator.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DMEMORY_ALLOCATOR_TYPE=MEMORY_ALLOCATOR_LINEAR $< -o $@
check: all
	$(BUILD)/poolLinked$(EXE)
	$(BUILD)/poolLinear$(EXE)
	$(BUILD)/containersLinked$(EXE)
	$(BUILD)/containersLinear$(EXE)
	$(DEMO)
clean:
	$(RM) $(DEMO) $(TESTS)
