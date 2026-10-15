# regalloc：寄存器分配（构建 + 自检）
#
# 产物在仓库根目录 build/regalloc：libregalloc.a、regalloc（命令行）、regalloc-tests。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../../..
BUILD_DIR ?= $(ROOT_DIR)/build/regalloc
LIB       := $(BUILD_DIR)/libregalloc.a
TOOL      := $(BUILD_DIR)/regalloc
TESTS     := $(BUILD_DIR)/regalloc-tests

SOURCES := regalloc.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
# 共享 IR：`parseProgram` / `plainText` / `lowerQuads` 都在这里（合并库里各只有一份）
SHARED_SOURCES := ../ir/typed_ir.cpp
SHARED_OBJECTS := $(BUILD_DIR)/typed_ir.o
LIB_OBJECTS := $(OBJECTS) $(SHARED_OBJECTS)

# 三地址 IR（`ir/typed_ir.hpp`）与四元式模型（`core/transfer.hpp`）在合并库里各只有一份
INCLUDES   := -I. -I../ir -I../core
# 只给命令行工具定义：把 `regallocMain` 包成 `main`（合并库 `t1t` 不编这一段）
STANDALONE := -DTRANSFER_MODULE_STANDALONE_MAIN=1

MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"

.PHONY: all tool tests check clean

all: tool tests

tool: $(TOOL)
tests: $(TESTS)

$(LIB): $(LIB_OBJECTS) | $(BUILD_DIR)
	ar rcs $@ $^

$(BUILD_DIR)/typed_ir.o: ../ir/typed_ir.cpp ../ir/typed_ir.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp regalloc.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) -c $< -o $@

$(TOOL): main.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) $(STANDALONE) main.cpp $(LIB) -o $@

$(TESTS): test_regalloc.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) $(INCLUDES) test_regalloc.cpp $(LIB) -o $@

check: all
	$(TESTS)
	$(TOOL) --regs=4 examples/pressure.ra
	$(TOOL) --regs=2 examples/pressure.ra
	$(TOOL) --algorithm=linear --regs=3 examples/loop.ra
	$(TOOL) --regs=2 examples/copy.ra
	$(TOOL) --regs=3 examples/redef.ra
	$(TOOL) --regs=4 examples/call.ra

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
