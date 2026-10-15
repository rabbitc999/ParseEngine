# irbridge：三元式 ↔ 四元式 互转（构建 + 自检）
#
# 产物在仓库根目录 build/irbridge：libirbridge.a、irbridge（命令行）、irbridge-tests。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../../..
BUILD_DIR ?= $(ROOT_DIR)/build/irbridge
LIB       := $(BUILD_DIR)/libirbridge.a
TOOL      := $(BUILD_DIR)/irbridge
TESTS     := $(BUILD_DIR)/irbridge-tests

SOURCES := irbridge.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)

# 三地址 IR（`ir/typed_ir.hpp`）与四元式模型（`core/transfer.hpp`）在合并库里各只有一份
INCLUDES   := -I. -I../ir -I../core
# 只给命令行工具定义：把 `irbridgeMain` 包成 `main`（合并库 `t1t` 不编这一段）
STANDALONE := -DTRANSFER_MODULE_STANDALONE_MAIN=1

MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"

.PHONY: all tool tests check clean

all: tool tests

tool: $(TOOL)
tests: $(TESTS)

$(LIB): $(OBJECTS) | $(BUILD_DIR)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: %.cpp irbridge.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(TOOL): main.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $(STANDALONE) main.cpp $(LIB) -o $@

$(TESTS): test_irbridge.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) test_irbridge.cpp $(LIB) -o $@

check: all
	$(TESTS)
	$(TOOL) triples2quads examples/if_while.triples -o $(BUILD_DIR)/if_while.quads
	$(TOOL) quads2triples examples/if_while.quads -o $(BUILD_DIR)/if_while.triples
	$(TOOL) stats examples/if_while.quads

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
