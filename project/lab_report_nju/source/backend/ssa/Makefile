# ssa：SSA 与稀疏优化（构建 + 自检）
#
# 产物在仓库根目录 build/ssa：libssa.a、ssa（命令行）、ssa-tests、ssa-examples。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../../..
BUILD_DIR ?= $(ROOT_DIR)/build/ssa
LIB       := $(BUILD_DIR)/libssa.a
TOOL      := $(BUILD_DIR)/ssa
TESTS     := $(BUILD_DIR)/ssa-tests
SUITE     := $(BUILD_DIR)/ssa-examples

SOURCES := ssa.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
# 共享 IR：`parseProgram` / `plainText` / `lowerQuads` 都在这里（合并库里各只有一份）
SHARED_SOURCES := ../ir/typed_ir.cpp
SHARED_OBJECTS := $(BUILD_DIR)/typed_ir.o
LIB_OBJECTS := $(OBJECTS) $(SHARED_OBJECTS)

# 三地址 IR（`ir/typed_ir.hpp`）与四元式模型（`core/transfer.hpp`）在合并库里各只有一份
INCLUDES   := -I. -I../ir -I../core
# 只给命令行工具定义：把 `ssaMain` 包成 `main`（合并库 `t1t` 不编这一段）
STANDALONE := -DTRANSFER_MODULE_STANDALONE_MAIN=1

MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"

.PHONY: all tool tests examples check clean

all: tool tests examples
tool: $(TOOL)
tests: $(TESTS)
examples: $(SUITE)

$(LIB): $(LIB_OBJECTS) | $(BUILD_DIR)
	ar rcs $@ $^

$(BUILD_DIR)/typed_ir.o: ../ir/typed_ir.cpp ../ir/typed_ir.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp ssa.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(TOOL): main.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $(STANDALONE) main.cpp $(LIB) -o $@

$(TESTS): test_ssa.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) test_ssa.cpp $(LIB) -o $@

$(SUITE): example_suite.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) example_suite.cpp $(LIB) -o $@

check: all
	$(TESTS)
	$(SUITE) examples
	$(TOOL) --mode=summary examples/04_loop_swap.ssa
	$(TOOL) --mode=dom examples/03_const_branch.ssa
	$(TOOL) --mode=ssa examples/02_diamond.ssa

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
