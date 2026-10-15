# lztext（library/lztext）：构建 + 自检
#
#   make            构建 build/lztext 下的 lztext-tests / lztext-pack，并跑一遍自检
#   make pack       只构建命令行打包工具（构建期内嵌脚本要用它）
#   make check      自检（往返一致性 + 边界用例 + 随机语料模糊测试）
#   make clean
#
# 只有一个编译单元（lztext.cpp），不依赖任何第三方库。
# 源码是 UTF-8（含中文注释）。GCC 下钉住输入/执行字符集。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

ifeq ($(findstring g++,$(CXX)),g++)
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
endif

ROOT_DIR  := ../..
BUILD_DIR ?= $(ROOT_DIR)/build/lztext

# 平台判定：build-all.sh 会用 `OS=` 覆盖 $(OS)（让各工程统一走 POSIX shell），
# 所以不能只看 $(OS)。先看 uname，uname 不可用时再退回 $(OS)。
UNAME_S := $(shell uname -s 2>/dev/null)
WINDOWS :=
ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),)
WINDOWS := 1
endif
ifeq ($(WINDOWS),)
ifeq ($(OS),Windows_NT)
WINDOWS := 1
endif
endif

ifeq ($(WINDOWS),1)
EXE           := .exe
BUILD_DIR_WIN := $(subst /,\,$(BUILD_DIR))
MKDIR         := powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(BUILD_DIR_WIN)' | Out-Null"
CLEAN         := cmd /c if exist "$(BUILD_DIR_WIN)" rmdir /s /q "$(BUILD_DIR_WIN)"
else
EXE           :=
MKDIR         := mkdir -p "$(BUILD_DIR)"
CLEAN         := rm -rf "$(BUILD_DIR)"
endif

TESTS_BIN := $(BUILD_DIR)/lztext-tests$(EXE)
PACK_BIN  := $(BUILD_DIR)/lztext-pack$(EXE)

.PHONY: all tests pack check clean

all: tests pack

tests: $(TESTS_BIN)
	$(TESTS_BIN)

pack: $(PACK_BIN)

check: tests

$(TESTS_BIN): test_lztext.cpp lztext.cpp lztext.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_lztext.cpp lztext.cpp -o $@

$(PACK_BIN): lztext_pack.cpp lztext.cpp lztext.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. lztext_pack.cpp lztext.cpp -o $@

$(BUILD_DIR):
	$(MKDIR)

clean:
	$(CLEAN)
