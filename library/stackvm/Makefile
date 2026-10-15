# 栈式虚拟机库：构建 + 自检
#
#   make            构建 build/stackvm 下的 libstackvm.a、stackvm、stackvm-tests
#   make check      单元测试 + 跑一遍 examples/ 里的四元式示例
#   make clean
#
# 源码是 UTF-8（含中文注释与诊断）。GCC 下钉住输入/执行字符集。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
AR       ?= ar

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/stackvm
LIB       := $(BUILD_DIR)/libstackvm.a

# 大整数：`bint` 类型用仓库里的 library/big_number（bigint::BigInt）。
# 直接把 bigint.cpp 编进本库的归档里 —— 用法与 project/bigint-calc 一样（都是把源码
# 编进链接），但这里是**静态库**：归档里的对象只在被引用时才被链进去，所以其它
# `-lstackvm` 的工程（没用到 bint 的那些）不用改 local.mk，也不会多出符号。
# 大整数是**附加功能**：STACKVM_WITH_BIGINT=1（缺省）编进来，=0 就是"轻量构建"
# （不 include / 不链 big_number，`__vm_bint_*` 调了会报一句"这个构建没把大整数编进来"）。
STACKVM_WITH_BIGINT ?= 1
STACKVM_WITH_BIGFLOAT ?= $(STACKVM_WITH_BIGINT)
ifeq ($(STACKVM_WITH_BIGFLOAT),1)
ifneq ($(STACKVM_WITH_BIGINT),1)
$(error STACKVM_WITH_BIGFLOAT=1 requires STACKVM_WITH_BIGINT=1)
endif
endif
ifeq ($(STACKVM_WITH_BIGINT),1)
BIGNUM_EXTRA_INC := -I$(ROOT_DIR)/library/big_number
BIGNUM_EXTRA_OBJ := $(BUILD_DIR)/bigint.o
else
BIGNUM_EXTRA_INC :=
BIGNUM_EXTRA_OBJ :=
endif
ifeq ($(STACKVM_WITH_BIGFLOAT),1)
BIGNUM_EXTRA_OBJ += $(BUILD_DIR)/bigfloat.o
endif
BIGNUM_DIR := $(ROOT_DIR)/library/big_number
BIGNUM_SRC := $(BIGNUM_DIR)/bigint.cpp
BIGNUM_OBJ := $(BUILD_DIR)/bigint.o
BIGFLOAT_SRC := $(BIGNUM_DIR)/bigfloat.cpp
BIGFLOAT_OBJ := $(BUILD_DIR)/bigfloat.o
CLI       := $(BUILD_DIR)/stackvm
TESTS     := $(BUILD_DIR)/stackvm-tests
BENCH     := $(BUILD_DIR)/stackvm-benchmark

# Windows 用 PowerShell 建目录、cmd 清理；其余平台用 POSIX 写法。
ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all lib cli tests benchmark check clean

all: $(CLI) $(TESTS)

lib: $(LIB)

cli: $(CLI)

tests: $(TESTS)

benchmark: $(BENCH)

$(LIB): $(BUILD_DIR)/stackvm.o $(BUILD_DIR)/stackvm_glue.o $(BIGNUM_EXTRA_OBJ) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(CLI): $(LIB) main.cpp stackvm.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(LIB) -o $@

$(TESTS): $(LIB) test_stackvm.cpp stackvm.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_stackvm.cpp $(LIB) -o $@

$(BENCH): $(LIB) benchmark_stackvm.cpp stackvm.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. benchmark_stackvm.cpp $(LIB) -o $@

$(BUILD_DIR)/stackvm.o: stackvm.cpp stackvm.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -DSTACKVM_WITH_BIGINT=$(STACKVM_WITH_BIGINT) -DSTACKVM_WITH_BIGFLOAT=$(STACKVM_WITH_BIGFLOAT) -I. $(BIGNUM_EXTRA_INC) -c $< -o $@

$(BUILD_DIR)/stackvm_glue.o: stackvm_glue.cpp stackvm_glue.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

$(BIGNUM_OBJ): $(BIGNUM_SRC) $(BIGNUM_DIR)/bigint.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -I$(BIGNUM_DIR) -c $(BIGNUM_SRC) -o $@

$(BIGFLOAT_OBJ): $(BIGFLOAT_SRC) $(BIGNUM_DIR)/bigfloat.hpp $(BIGNUM_DIR)/bigint.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -I$(BIGNUM_DIR) -c $(BIGFLOAT_SRC) -o $@

check: all
	$(TESTS) examples
	$(CLI) run examples/08_call.quad --stats
	$(CLI) disasm examples/minimal.quad
	$(CLI) check examples/07_io.quad
	$(CLI) compile examples/minimal.quad --out $(BUILD_DIR)/minimal.svmb
	$(CLI) check $(BUILD_DIR)/minimal.svmb

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
