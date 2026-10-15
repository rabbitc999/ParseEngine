# dataflow：通用数据流框架（构建 + 自检）
#
# 产物在仓库根目录 build/dataflow：
#   libdataflow.a、dataflow（命令行）、dataflow-tests、dataflow-examples。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../../..
BUILD_DIR ?= $(ROOT_DIR)/build/dataflow
LIB       := $(BUILD_DIR)/libdataflow.a
TOOL      := $(BUILD_DIR)/dataflow
TESTS     := $(BUILD_DIR)/dataflow-tests
SUITE     := $(BUILD_DIR)/dataflow-examples

SOURCES := dataflow.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
# 共享 IR：`parseProgram` / `plainText` / `lowerQuads` 都在这里（合并库里各只有一份）
SHARED_SOURCES := ../ir/typed_ir.cpp
SHARED_OBJECTS := $(BUILD_DIR)/typed_ir.o
LIB_OBJECTS := $(OBJECTS) $(SHARED_OBJECTS)

# 三地址 IR（`ir/typed_ir.hpp`）与四元式模型（`core/transfer.hpp`）在合并库里各只有一份
INCLUDES   := -I. -I../ir -I../core
# 只给命令行工具定义：把 `dataflowMain` 包成 `main`（合并库 `t1t` 不编这一段）
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

$(BUILD_DIR)/%.o: %.cpp dataflow.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(TOOL): main.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $(STANDALONE) main.cpp $(LIB) -o $@

$(TESTS): test_dataflow.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) test_dataflow.cpp $(LIB) -o $@

$(SUITE): example_suite.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) example_suite.cpp $(LIB) -o $@

check: all
	$(TESTS)
	$(SUITE) examples
	$(TOOL) --mode=check examples/01_diamond.df
	$(TOOL) --analysis=live --mode=sets examples/02_loop.df
	$(TOOL) --analysis=avail --mode=mermaid examples/03_avail.df
	$(TOOL) --analysis=reach --mode=summary --order=lifo --seed=all examples/02_loop.df

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
