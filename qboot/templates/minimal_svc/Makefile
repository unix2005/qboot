# 最小服务模板 —— 直接用安装后的 qboot 库编译
#
#   export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig   # 按需修改
#   make            # 静态链接（默认，产物自包含，部署不用带 .so）
#   make shared     # 动态链接（产物小，运行时需要 qboot .so）
#   make run        # 编译并运行
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall
PKGCONF ?= pkg-config

TARGET   = minimal_svc
SRCS     = minimal_svc.c

# 依赖者在前，被依赖者在后 —— GNU ld 对静态库顺序敏感
QBOOT_LIBS := q_http q_mapper q_reg q_httpc q_db_mysql q_db_mock q_db q_conf q_log q_core

QBOOT_CFLAGS := $(shell $(PKGCONF) --cflags qboot)
QBOOT_LIBDIR := $(shell $(PKGCONF) --variable=libdir qboot)
QBOOT_LDFLAGS_SHARED := $(shell $(PKGCONF) --libs qboot)

# 静态链接只需要"系统依赖那部分"（libevent / curl / libxml2 / jansson / mariadb ...）。
# pkg-config --static --libs 会把 -L、-lq_xxx、系统库一起吐出来，
# 用 filter-out 把 qboot 自己的 -l 去掉，只留下系统库，
# 否则 ld 会优先挑同目录的 .so，"静态"就名不副实了。
QBOOT_LDFLAGS_PRIVATE := $(filter-out -lq_%,$(shell $(PKGCONF) --static --libs qboot 2>/dev/null))

# 静态链接必须用 .a 的全路径：
# 只写 -lq_http 时，ld 会优先挑同目录下的 .so，那就不是静态链接了。
# GNU ld 上也可以用 -Wl,-Bstatic ... -Wl,-Bdynamic，但 macOS 的 ld64 不支持，
# 全路径在两边都成立，这里统一用全路径。
QBOOT_STATIC_LIBS := $(foreach l,$(QBOOT_LIBS),$(QBOOT_LIBDIR)/lib$(l).a)

static: $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) \
	    $(QBOOT_CFLAGS) $(QBOOT_STATIC_LIBS) $(QBOOT_LDFLAGS_PRIVATE)

shared: $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET)_shared $(SRCS) \
	    $(QBOOT_CFLAGS) $(QBOOT_LDFLAGS_SHARED) \
	    -Wl,-rpath,$(QBOOT_LIBDIR)

run: static
	./$(TARGET) app.ini

clean:
	rm -f $(TARGET) $(TARGET)_shared

.PHONY: static shared run clean
