# ==============================================================================
# 变量定义 (Varaibles)
# ==============================================================================

# 编译器
CC := gcc

# 编译选项
# -g: 添加调试信息
# -Wall -Wextra: 开启所有常用和额外的警告
# -Iinclude: 告诉编译器去 'include' 目录下查找头文件 (.h)
# -pthread: 启用 POSIX 线程支持
CFLAGS := -g -Wall -Wextra -Iinclude -pthread

# 链接选项
# -pthread: 链接时加入线程库
# -lz: 链接 zlib 压缩库
LDFLAGS := -pthread -lz

# 目录定义
SRCDIR := src
INCDIR := include
OBJDIR := obj
BINDIR := bin

# ==============================================================================
# 自动文件列表 (Automatic File Lists)
# ==============================================================================

# 目标可执行文件名
TARGET := listfiles

# 自动获取 src 目录下所有的 .c 文件
# SOURCES := $(filter-out $(SRCDIR)/smart_queue.c, $(wildcard $(SRCDIR)/*.c))
SOURCES :=  $(wildcard $(SRCDIR)/*.c)
# 根据 .c 文件列表，自动生成对应的 .o (object) 文件列表
# 例如, src/main.c 会被转换为 obj/main.o
OBJECTS := $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# ==============================================================================
# 规则定义 (Rules)
# ==============================================================================

# 默认规则：第一个规则是 'make' 命令默认执行的规则
# 依赖于最终的可执行文件
all: $(BINDIR)/$(TARGET)

# 链接规则: 将所有的 .o 文件链接成最终的可执行文件
# $@ 代表规则的目标 (即 bin/listfiles)
# $^ 代表规则的所有依赖 (即所有的 .o 文件)
$(BINDIR)/$(TARGET): $(OBJECTS)
	@mkdir -p $(BINDIR)
	@echo "===> Linking..."
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo "===> Build complete! Executable is at: $(BINDIR)/$(TARGET)"

# 编译规则: 定义了如何从一个 .c 文件编译成一个 .o 文件
# $< 代表规则的第一个依赖 (即对应的 .c 文件)
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	@echo "===> Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# 清理规则: 删除所有生成的文件
clean:
	@echo "===> Cleaning up..."
	@rm -rf $(OBJDIR) $(BINDIR)

# 伪目标: 告诉 make 'all' 和 'clean' 不是真正的文件名
.PHONY: all clean