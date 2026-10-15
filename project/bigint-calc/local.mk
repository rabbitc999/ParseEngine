# local.mk —— 生成工程自己的挂载点（生成的 Makefile 末尾有 `-include local.mk`）。
#
# 本工程用仓库里的 library/big_number（任意精度整数，只依赖标准库）：
#   * 头文件走 -I（%code ... %endcode 里的 #include "bigint.hpp"）
#   * 源码直接进链接行——只有 bigint.cpp 一个文件，不值得单独建静态库
#
# 注意：这个文件被 include 的位置是**生成目录**（build/），所以路径从 build/
# 起算：build → bigint-calc → project → 仓库根，三层上去。

BIGNUM_DIR := ../../../library/big_number

CXXFLAGS += -I$(BIGNUM_DIR)
LDLIBS   += $(BIGNUM_DIR)/bigint.cpp

# Windows 上 make 找不到 sh：优先用 Git Bash / MSYS2 的 bash（与其它工程一致）。
ifeq ($(OS),Windows_NT)
  SH ?= $(firstword $(wildcard C:/PROGRA~1/Git/bin/bash.exe) \
                    $(wildcard C:/PROGRA~2/Git/bin/bash.exe) \
                    $(wildcard C:/msys64/usr/bin/bash.exe) sh)
else
  SH ?= sh
endif

.PHONY: check samples

# 自检：跑 samples/ 与 samples-error/ 下的样例，比对 expected/。
check: $(TARGET)
	$(SH) ../check.sh

# 只跑样例、不重新编译（改样例 / 期望时用）。
samples: $(TARGET)
	$(SH) ../check.sh
