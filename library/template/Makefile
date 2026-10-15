# 模板引擎（library/template）：构建 + 自检
#
#   make            构建 build/template 下的 template-tests、template-demo 并跑一遍
#   make check      自检（单元测试）
#   make clean
#
# 头文件-only：除了 demo 没有别的编译单元。
# 源码是 UTF-8（含中文注释）。GCC 下钉住输入/执行字符集。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/template
DEMO      := $(BUILD_DIR)/template-demo
TESTS     := $(BUILD_DIR)/template-tests

# Windows 用 PowerShell 建目录、cmd 清理；其余平台用 POSIX 写法。
ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all tests demo check clean

all: tests demo

tests: $(TESTS)

demo: $(DEMO)

$(TESTS): test_template.cpp value.hpp bind.hpp model.hpp engine.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_template.cpp -o $@
	$(TESTS)

check: tests demo

$(DEMO): main.cpp value.hpp bind.hpp model.hpp engine.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp -o $@
	$(DEMO)

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
