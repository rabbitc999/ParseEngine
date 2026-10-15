# JSON 库：构建 + 自检
#
#   make            构建 build/json 下的 libjson.a、json、json-tests、model-demo
#   make check      单元测试 + 用 CLI 跑一遍 examples/config.json
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
BUILD_DIR ?= $(ROOT_DIR)/build/json
LIB       := $(BUILD_DIR)/libjson.a
# MinGW 即使 `-o json` 也会实际写出 json.exe。目标名不带后缀时，make 每次都
# 认为文件不存在并重新链接三个程序；ComSpec 在 Windows 的 cmd/PowerShell/Git Bash
# 中都存在，比会被根构建脚本覆盖的 OS 变量更可靠。
EXEEXT    := $(if $(or $(ComSpec),$(COMSPEC)),.exe,)
CLI       := $(BUILD_DIR)/json$(EXEEXT)
TESTS     := $(BUILD_DIR)/json-tests$(EXEEXT)
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

$(LIB): $(BUILD_DIR)/json.o | $(BUILD_DIR)
	$(AR) rcs $@ $<

$(CLI): $(LIB) main.cpp json.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(LIB) -o $@

$(TESTS): $(LIB) test_json.cpp json.hpp bind.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_json.cpp $(LIB) -o $@

$(DEMO): $(LIB) examples/model_demo.cpp json.hpp bind.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. examples/model_demo.cpp $(LIB) -o $@

$(BUILD_DIR)/json.o: json.cpp json.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

check: all
	$(TESTS)
	$(CLI) fmt examples/config.json --compact --out $(BUILD_DIR)/config.compact.json
	$(CLI) check $(BUILD_DIR)/config.compact.json
	$(CLI) get limits.maxDepth examples/config.json
	$(CLI) keys $(BUILD_DIR)/config.compact.json
	$(CLI) stats examples/config.json
	$(DEMO) examples/config.json

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
