# simple_ast：构建 + 自检
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
AR       ?= ar
ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif
ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/simple_ast
LIB       := $(BUILD_DIR)/libsimple_ast.a
CLI       := $(BUILD_DIR)/simple_ast
TESTS     := $(BUILD_DIR)/simple_ast-tests
ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"
endif
.PHONY: all lib cli tests check clean
all: $(CLI) $(TESTS)
lib: $(LIB)
cli: $(CLI)
tests: $(TESTS)
$(LIB): $(BUILD_DIR)/simple_ast.o | $(BUILD_DIR)
	$(AR) rcs $@ $<
$(CLI): $(LIB) main.cpp simple_ast.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(LIB) -o $@
$(TESTS): $(LIB) test_simple_ast.cpp simple_ast.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_simple_ast.cpp $(LIB) -o $@
$(BUILD_DIR)/simple_ast.o: simple_ast.cpp simple_ast.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@
check: all
	$(TESTS)
	$(CLI) $(BUILD_DIR)/demo
$(BUILD_DIR):
	$(MKDIR)
clean:
	$(CLEAN)
