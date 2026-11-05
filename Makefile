# ============================================================
# EMDA (Embedded Module Device AT) Makefile
# ============================================================
# 常用命令：
#   make                              用默认 arm-none-eabi-gcc 编译静态库
#   make CC=gcc AR=ar                 切回主机 gcc 做语法验证
#   make test                         主机 gcc 编译并运行模拟模组测试
#   make clean                        清理 build/
# ============================================================

# ---- 工具链（默认 arm 交叉编译）----
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
HOSTCC  ?= gcc

CFLAGS  ?= -Wall -Wextra -Wno-unused-parameter -std=c99 -O2 -g
ARFLAGS  = rcs

# ---- 路径 ----
SRC_DIR   := source
CLASS_DIR := $(SRC_DIR)/class
TEST_DIR  := test
BUILD_DIR := build

# ---- 库源码 ----
# 顺序体现分层：port(移植) -> at(事务) -> socket(数据面) -> device(控制面) -> class(模组)
# 注意：example/ 下的示例依赖 RTOS 头文件且含 main，不纳入构建。
LIB_SRCS := $(SRC_DIR)/emda_port.c \
            $(SRC_DIR)/emda_at.c \
            $(SRC_DIR)/emda_socket.c \
            $(SRC_DIR)/emda_device.c \
            $(CLASS_DIR)/emda_class_ml307a.c

# 目标文件统一平铺到 build/，避免依赖 sh 的 mkdir -p 建子目录
LIB_OBJS := $(addprefix $(BUILD_DIR)/,$(notdir $(LIB_SRCS:.c=.o)))

INCLUDES := -I$(SRC_DIR) -I$(CLASS_DIR)

LIB     := $(BUILD_DIR)/libemda.a

# ---- 主机测试 ----
# 用 test/fake_modem.c 提供 emda_port 实现（因此不能链接 emda_port.c）
TEST_SRCS := $(SRC_DIR)/emda_at.c \
             $(SRC_DIR)/emda_socket.c \
             $(SRC_DIR)/emda_device.c \
             $(CLASS_DIR)/emda_class_ml307a.c \
             $(TEST_DIR)/fake_modem.c \
             $(TEST_DIR)/test_main.c

TEST_BIN  := $(BUILD_DIR)/emda_test

# ---- 跨平台建目录/删目录 ----
ifeq ($(OS),Windows_NT)
  MKDIR = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
  RMDIR = if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
else
  MKDIR = mkdir -p "$(1)"
  RMDIR = rm -rf "$(1)"
endif

# 源文件搜索路径（支持 source/ 与 source/class/ 同名规则）
vpath %.c $(SRC_DIR) $(CLASS_DIR)

# ============================================================
# 规则
# ============================================================

.PHONY: all lib test clean

all: lib

# 静态库
lib: $(LIB)

$(LIB): $(LIB_OBJS) | $(BUILD_DIR)
	$(AR) $(ARFLAGS) $@ $^

# ---- 编译规则 ----
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---- 主机测试：编译后在 PC 上直接跑 ----
test: | $(BUILD_DIR)
	$(HOSTCC) -std=c99 -Wall -Wextra -Wno-unused-parameter \
		$(INCLUDES) -I$(TEST_DIR) $(TEST_SRCS) -o $(TEST_BIN)
	./$(TEST_BIN)

# ---- 目录 ----
$(BUILD_DIR):
	@$(call MKDIR,$(BUILD_DIR))

# ---- 清理 ----
clean:
	@$(call RMDIR,$(BUILD_DIR))
