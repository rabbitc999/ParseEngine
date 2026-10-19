# `.gram` 语言服务器：构建 + 自检
#
# 产物在仓库根目录的 build/lsp：lsp-server（服务器）与 lsp-tests（单元测试）。
#   make            # 构建
#   make check      # 单元测试 + 示例会话（examples/client.py 驱动真服务器）
#   make clean
#
# 源码是 UTF-8（中文注释与中文诊断）。GCC 下钉住输入/执行字符集，MSVC 用 /utf-8。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
PYTHON   ?= python

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/lsp
SERVER    := $(BUILD_DIR)/lsp-server
TESTS     := $(BUILD_DIR)/lsp-tests

SOURCES := json.cpp grammar.cpp lsp.cpp
OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)

# Windows 用 PowerShell 建目录、cmd 清理；其余平台用 POSIX 写法。
ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all server tests check clean

all: server tests

server: $(SERVER)

tests: $(TESTS)

$(SERVER): $(OBJECTS) main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(OBJECTS) -o $@

$(TESTS): $(OBJECTS) test_lsp.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_lsp.cpp $(OBJECTS) -o $@

$(BUILD_DIR)/%.o: %.cpp json.hpp types.hpp grammar.hpp lsp.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

check: all
	$(TESTS)
	$(PYTHON) examples/client.py $(SERVER) examples/sample.gram
	$(PYTHON) examples/client.py $(SERVER) examples/sample_broken.gram --expect-errors

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)

# LSP 的接收与分析线程使用标准线程库。
CXXFLAGS += -pthread
LDFLAGS += -pthread
