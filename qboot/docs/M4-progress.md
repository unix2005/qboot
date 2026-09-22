# M4：工程化 —— 库双产物 + install/export + demo + 测试脚本

目标（用户原话）：「继续做好工程化 模块化，能编译成 .a .so 的，都要编译成 .a .so，
方便后续开发微服务。本机能有的环境就测试，没有的我明天 copy 到 ubuntu 上测，
给我写好详细的测试脚本和使用手册，做好 demo。」

## 1. 每个库同时产出 .a 和 .so

做法：静态库是"主"目标，`q_derive_shared()` 从它的 SOURCES 自动派生一个
`<name>_shared` 动态库，`OUTPUT_NAME` 保持一致，于是磁盘上同时有
`libq_x.a` 和 `libq_x.so`。

踩到的坑：

- **`get_target_property(SOURCES)` 返回的是相对路径，且相对于定义该 target 的目录**。
  在顶层 CMakeLists 里直接用会解析不到，报 `No SOURCES given`。
  必须按 `get_target_property(src_dir ... SOURCE_DIR)` 补全成绝对路径。
- 依赖必须跟着换成 `_shared`（`q_log -> q_log_shared`），
  否则 `libq_log.so` 会把 `libq_core.a` 静态链进去，应用再链 `libq_core.so`
  就出现两套符号。
- 静态库必须 `-fPIC`（`CMAKE_POSITION_INDEPENDENT_CODE ON`），否则链不进 `.so`。
- 动态库加 `VERSION/SOVERSION`，产生 `libq_x.so -> .so.0 -> .so.0.3.0` 三级软链。

## 2. llhttp 收进 libq_http

原来 `q_llhttp` 是独立静态库，但它没被导出，外部用 `find_package(qboot)` 链接时
报 `undefined reference to llhttp_*`。它是实现细节，不该让使用者单独链接，
所以把 `deps/llhttp/{llhttp,api,http}.c` 直接编进 `q_http`
（用 `set_source_files_properties(... COMPILE_OPTIONS "-w")` 只对这三个文件关警告）。

## 3. install / export

```
include/q/**.h
lib/libq_*.a   lib/libq_*.so*
lib/pkgconfig/qboot.pc
lib/cmake/qboot/qboot-config.cmake
bin/q_upstream_sync
share/qboot/deploy/nginx/*.conf
share/qboot/templates/**
share/qboot/sql/*.sql
```

- `qboot.pc` 用 `prefix=${pcfiledir}/../..`，跟着实际安装位置走，
  `cmake --install --prefix=xxx` 换目录不失效。
- `Libs.private` 里带 `-L`：**只写 `-l` 在 brew keg-only 场景下会找不到库**
  （`libmariadb` 在 `/usr/local/opt/mariadb-connector-c/lib`）。
  同时过滤掉 Xcode SDK 路径（只有 .tbd，写进去反而污染）。
- `qboot-config.cmake` 里的依赖表**必须用空格分隔条目**，不能用分号——
  CMake 的 list 以分号分隔，写 `"q_log;q_core"` 会被拆成两个条目。
- 每个库导出两个目标：`qboot::q_core`（.a）和 `qboot::q_core_shared`（.so）。

## 4. 静态链接的"陷阱"

`pkg-config --static --libs qboot` 会把 `Libs` 和 `Libs.private` 一起吐出来，
也就是又带回了 `-lq_http`。ld 优先挑同目录的 `.so`，于是"静态"名不副实
（`otool -L` 一看 10 个 dylib 依赖）。

模板 Makefile 的处理：

```make
QBOOT_STATIC_LIBS   = $(foreach l,$(QBOOT_LIBS),$(QBOOT_LIBDIR)/lib$(l).a)   # 全路径
QBOOT_LDFLAGS_PRIVATE = $(filter-out -lq_%,$(shell pkg-config --static --libs qboot))
```

全路径在 GNU ld 和 macOS ld64 上都成立（`-Wl,-Bstatic` 后者不支持）。
验证：静态产物 `otool -L | grep -c libq_` = **0**，动态产物 = 依赖齐全。

## 5. demo

- `samples/order_svc.c` + `samples/mapper/Order.xml` + `samples/conf/order_svc.ini`：
  演示服务间调用。`call_user_svc()` 两段式——有 Consul 走 `q_reg_call("user-svc", ...)`，
  没有就退化成配置里的直连地址，业务代码只认服务名。
- `user_svc` / `order_svc` 在 `mock://` 下用 `qmock_expect()` 预置结果集，
  **没有数据库也能端到端跑通**。
- `templates/minimal_svc/`：60 行骨架 + Makefile + CMakeLists，两种接入方式都能编译运行。

## 6. 脚本

`scripts/common.sh`（参数解析 / 断言 / wait_port / 后台进程回收）+ 6 个脚本。
都做了：出错立即停 + 打印可复制的修复命令 + 退出自动收进程。

两个自己踩的坑，写进注释了：

1. `exec 3<&- 2>/dev/null` —— `exec` 的重定向对当前 shell **永久生效**，
   这一句把脚本的 stderr 丢进了 /dev/null，后面所有报错都消失，只剩退出码 1。
   改成在子 shell 里开 fd。
2. `( cd x && bg_start ... )` —— 子 shell 里的 `Q_PIDS` 是副本，
   父脚本收不到 pid，进程泄漏。改成先 `cd` 再 `bg_start`。

另外为了兼容 macOS 自带的 bash 3.2（没有关联数组），参数解析用平行数组实现，
这样本机也能跑通这些脚本。

## 7. 修掉的真 bug

**`json_ok()` 之后又 `json_decref()` → use-after-free。**
`json_object_set_new` 会接管引用所有权。症状是服务偶发 `Abort trap: 6`，
ASan 定位到 `h_search` / `h_list`。已在两个 sample 里修掉并写清所有权约定。

## 8. 验证结果（macOS）

- 构建：10 个 `.a` + 10 组 `.dylib`（含三级软链）
- 单元测试：core 44 / conf 12 / log 4 / mapper 49 / reg 28 = **137 全过**，ASan 同样全过
- install 后外部接入：find_package（静态/动态）、pkg-config、手写 `-l` 三种都通
- `scripts/run_demo.sh` 连跑三次退出码 0；跨服务调用、建单、查询都返回预期 JSON
- Ubuntu 上待验证：真库冒烟（`smoke_mysql.sh`）、Consul 冒烟（`smoke_consul.sh`）、
  nginx 接入
