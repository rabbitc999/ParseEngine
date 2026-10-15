# TOML 库：构建 + 自检
#
#   make            构建 build/toml 下的 libtoml.a、toml、toml-tests、model-demo
#   make check      单元测试 + 用 CLI 跑一遍 examples/config.toml
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
BUILD_DIR ?= $(ROOT_DIR)/build/toml
LIB       := $(BUILD_DIR)/libtoml.a
# MinGW 即使 `-o toml` 也会实际写出 toml.exe。目标名不带后缀时，make 每次都
# 认为文件不存在并重新链接三个程序；ComSpec 在 Windows 的 cmd/PowerShell/Git Bash
# 中都存在，比会被根构建脚本覆盖的 OS 变量更可靠。
EXEEXT    := $(if $(or $(ComSpec),$(COMSPEC)),.exe,)
CLI       := $(BUILD_DIR)/toml$(EXEEXT)
TESTS     := $(BUILD_DIR)/toml-tests$(EXEEXT)
DEMO      := $(BUILD_DIR)/model-demo$(EXEEXT)

# Windows 用 PowerShell 建目录、cmd 清理；其余平台用 POSIX 写法。
ifeq ($(OS),Windows_NT)
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
MKDIR := mkdir -p "$(BUILD_DIR)"
CLEAN := rm -rf "$(BUILD_DIR)"
endif

.PHONY: all lib cli tests demo check clean

all: $(CLI) $(TESTS) $(DEMO)

lib: $(LIB)

cli: $(CLI)

tests: $(TESTS)

demo: $(DEMO)

$(LIB): $(BUILD_DIR)/toml.o | $(BUILD_DIR)
	$(AR) rcs $@ $<

$(CLI): $(LIB) main.cpp toml.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(LIB) -o $@

$(TESTS): $(LIB) test_toml.cpp toml.hpp bind.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_toml.cpp $(LIB) -o $@

$(DEMO): $(LIB) examples/model_demo.cpp toml.hpp bind.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. examples/model_demo.cpp $(LIB) -o $@

$(BUILD_DIR)/toml.o: toml.cpp toml.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

check: all
	$(TESTS)
	$(CLI) fmt examples/config.toml --out $(BUILD_DIR)/config.fmt.toml
	$(CLI) check $(BUILD_DIR)/config.fmt.toml
	$(CLI) get limits.maxDepth examples/config.toml
	$(CLI) keys $(BUILD_DIR)/config.fmt.toml
	$(CLI) stats examples/config.toml
	$(DEMO) examples/config.toml

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
