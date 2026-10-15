# 编译原理实验平台学生使用手册

- 类型：平台说明
- 来源整理：`OJ.md`
- 版本：1.2.3
- 处理：已删除目录页、页码及重复页眉页脚，并重排命令与目录结构。

## 本节位置

```mermaid
flowchart LR
    A["实验五框架"] --> B["OJ 平台"] --> C["提交要求"]
```

## 版本记录

| 版本 | 日期 | 作者 | 变更 |
|---|---|---|---|
| 1.0.0 | 2022-09-14 | 张腾 | 新增用户、实验章节 |
| 1.1.0 | 2022-09-20 | 张腾 | 新增编译原理实验注意事项 |
| 1.1.1 | 2023-02-17 | 张腾 | 补充 Python 安装及 irsim 版本区别 |
| 1.2.0 | 2024-02-25 | 张腾 | 新增提交准备事项与账号注册说明 |
| 1.2.1 | 2024-02-29 | 张腾 | 更新实验环境镜像源命令 |
| 1.2.2 | 2024-03-26 | 张腾 | 更新图片 |
| 1.2.3 | 2025-02-17 | 孙伟杰 | 使用 Python 3 执行 irsim 命令行版 |

## 1 前言

### 1.1 平台介绍

课程 OJ 用于实验提交与自动评测。平台地址：<https://compiler.pascal-lab.net/>。

### 1.2 提交实验前的三项准备

1. 加入选课系统或课程网站公布的课程 QQ 群，及时了解实验动态。
2. 填写助教在群内提供的实验分组登记表。
3. 阅读本手册，确认账号注册、实验打包和提交规范。

### 1.3 编译原理实验注意事项

#### 1.3.1 学生实验环境

学生环境应尽量与 OJ 评测环境一致。使用不同环境产生的兼容问题需要自行排查。

| 项目 | 要求 |
|---|---|
| CPU | AMD x86_64 架构 |
| 内存 | 不少于 1 GB |
| 操作系统 | Ubuntu 20.04 64 位 |
| C 编译器 | GCC 7.5.0 |
| 词法分析器生成器 | Flex 2.6.4 |
| 语法分析器生成器 | Bison 3.5.1 |
| MIPS 模拟器 | SPIM 8.0 |
| Python | Python 3.8，用于执行 irsim |

可选：将 Ubuntu 20.04 软件源切换到南京大学镜像。

```bash
cp /etc/apt/sources.list /etc/apt/sources.list.bak

cat <<'EOF' > /etc/apt/sources.list
deb http://mirror.nju.edu.cn/ubuntu/ focal main restricted universe multiverse
deb http://mirror.nju.edu.cn/ubuntu/ focal-updates main restricted universe multiverse
deb http://mirror.nju.edu.cn/ubuntu/ focal-backports main restricted universe multiverse
deb http://mirror.nju.edu.cn/ubuntu/ focal-security main restricted universe multiverse
EOF
```

安装课程所需软件，并将 GCC 7 设为默认版本：

```bash
apt update
apt install -qq -y build-essential flex bison libbison-dev gcc-7 python3.8 python3-pip python2-minimal
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-7 100
```

检查工具版本：

```bash
uname -a
gcc --version
flex --version
bison --version
```

内核补丁版本不要求完全一致；GCC、Flex 与 Bison 的主要版本应符合课程要求。

#### 1.3.2 实验编译方式

以课程主页提供的 `Makefile` 为准，不要修改评测约定。实验使用 C 语言实现，不应依赖 Flex 和 Bison 之外的第三方库。

#### 1.3.3 实验输出格式

实验一和实验二的错误信息、语法树均通过标准输出 `stdout` 输出，不要写入标准错误 `stderr`。程序应正常退出，即返回值为 `0`。

#### 1.3.4 IR 虚拟机

`irsim` 包含命令行版和图形界面版：命令行版供测试脚本自动执行 IR 文件，图形界面版供学生自测和调试。

## 2 用户模块

### 2.1 注册账号

每个实验队伍由队长注册一个账号，再将账号密码分享给队员。队员不要重复注册，以免影响组队成绩关联。

1. 打开 OJ 网站，点击右上角的 **Register**。
2. 填写信息并通过邮箱验证码完成注册。

| 字段 | 填写内容 |
|---|---|
| Username | 学号 |
| Nickname | 姓名 |
| Email | `学号@smail.nju.edu.cn`，不要使用别名邮箱 |

### 2.2 登录

打开 OJ 网站，点击右上角的 **Login**，填写账号和密码后登录。

## 3 实验模块

### 3.1 实验列表

在网站主页的实验列表中选择实验。点击具体题目后进入题目详情页。

### 3.2 实验详情页

实验详情页包含三个标签页：

- **Overview**：显示题目数量及题号、名称等基本信息。
- **Problem**：显示题目描述、时空限制、最近提交记录，并提供题目和评测模板切换功能。
- **Status**：显示当前用户的提交及评测结果；点击评测 ID 可进入提交详情页。

### 3.3 提交实验

按照实验要求制作 ZIP 压缩包。正确结构示例：

```text
张三_1234567.zip
├── Code
│   ├── lexical.l
│   ├── main.c
│   ├── Makefile
│   └── ...
├── parser
├── README
└── report.pdf
```

不要在压缩包中额外嵌套同名目录。以下结构是错误的：

```text
张三_1234567.zip
└── 张三_1234567
    ├── Code
    │   └── ...
    ├── parser
    ├── README
    └── report.pdf
```

进入题目的 **Problem** 页面，点击 **Upload** 选择压缩包，再点击 **Submit** 完成提交。
