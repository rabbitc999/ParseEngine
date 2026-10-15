# 预处理库：构建 + 自检
#
#   make            构建 build/preprocess 下的 libpreprocess.a、preprocess、preprocess-tests
#   make check      单元测试 + 跑一遍 examples/ 里的示例
#   make clean
#
# 源码是 UTF-8（含中文注释与诊断）。GCC 下钉住输入/执行字符集，MSVC 用 /utf-8。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
AR       ?= ar

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/preprocess
LIB       := $(BUILD_DIR)/libpreprocess.a
CLI       := $(BUILD_DIR)/preprocess
TESTS     := $(BUILD_DIR)/preprocess-tests

# Windows 用 PowerShell 建目录、cmd 清理；其余平台用 POSIX 写法。
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

$(LIB): $(BUILD_DIR)/preprocess.o | $(BUILD_DIR)
	$(AR) rcs $@ $<

$(CLI): $(LIB) main.cpp preprocess.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(LIB) -o $@

$(TESTS): $(LIB) test_preprocess.cpp preprocess.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_preprocess.cpp $(LIB) -o $@

$(BUILD_DIR)/preprocess.o: preprocess.cpp preprocess.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

check: all
	$(TESTS) examples
	$(CLI) examples/main.pp -D DEBUG -o $(BUILD_DIR)/example-debug.out --stats
	$(CLI) examples/main.pp --check --stats

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
