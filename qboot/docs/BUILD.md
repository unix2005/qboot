# qboot 构建与安装手册

## 0. 一句话

```bash
sudo ./scripts/install_deps_ubuntu.sh     # Ubuntu 装依赖（只做一次）
./scripts/build.sh                        # 编译 + 安装到 /usr/local
./scripts/test_all.sh                     # 跑测试，应该 137 项全过
```

## 1. 依赖

| 依赖 | 用途 | Ubuntu / Debian | macOS |
|---|---|---|---|
| cmake ≥ 3.20 | 构建 | `apt install cmake` | `brew install cmake` |
| C 编译器 | 构建 | `apt install build-essential` | Xcode CLT |
| pkg-config | 生成 `qboot.pc` | `apt install pkg-config` | `brew install pkg-config` |
| libevent | libq_http（IO） | `apt install libevent-dev` | `brew install libevent` |
| libcurl | libq_httpc（出站 HTTP） | `apt install libcurl4-openssl-dev` | 系统自带 |
| libxml2 | libq_mapper（XML） | `apt install libxml2-dev` | 系统自带（SDK） |
| jansson | libq_mapper / JSON | `apt install libjansson-dev` | `brew install jansson` |
| MariaDB Connector/C | libq_db_mysql | `apt install libmariadb-dev` | `brew install mariadb-connector-c` |

可选：

| 组件 | 用途 | 安装 |
|---|---|---|
| Consul | 注册发现 | `./scripts/install_deps_ubuntu.sh --with-consul` |
| MariaDB Server | 真库冒烟 | `./scripts/install_deps_ubuntu.sh --with-mysql-server` |
| nginx | 接入层 demo | `./scripts/install_deps_ubuntu.sh --with-nginx` |

**依赖缺失不会让构建失败**：对应的库会被自动跳过（构建日志里有
`not found -> skip libq_xxx`）。例如没装 libevent 时 `libq_http` 不构建，
`libq_core` / `libq_log` / `libq_conf` / `libq_db` / `libq_mapper` 照常产出。

## 2. 编译

```bash
./scripts/build.sh                        # Release，装到 /usr/local
./scripts/build.sh --no-install           # 只编译，不安装
./scripts/build.sh --prefix /opt/qboot    # 换安装前缀
./scripts/build.sh --type Debug --clean   # Debug + 清空重编
./scripts/build.sh --asan                 # ASan 构建（查内存问题）
./scripts/build.sh --shared 0             # 只产出 .a
./scripts/build.sh --help                 # 全部选项
```

手工等价命令：

```bash
cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DQ_BUILD_SHARED=ON \
      -DQ_BUILD_HTTP=ON
cmake --build build -j$(nproc)
sudo cmake --install build
```

### 2.1 开关

| 选项 | 默认 | 说明 |
|---|---|---|
| `Q_BUILD_SHARED` | ON | 每个库**同时**产出 `.a` 和 `.so`（macOS 是 `.dylib`） |
| `Q_BUILD_HTTP` | ON | 构建 libq_http（需要 libevent） |
| `Q_BUILD_SAMPLES` | ON | 构建样例服务 |
| `Q_BUILD_TOOLS` | ON | 构建 q_upstream_sync |
| `Q_BUILD_TESTS` | ON | 构建单元测试 |
| `Q_ENABLE_ASAN` | OFF | AddressSanitizer |
| `Q_ENABLE_TSAN` | OFF | ThreadSanitizer |
| `Q_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |

> `option()` 有缓存。改过默认值之后必须显式传 `-Dxxx=ON/OFF` 才会生效，
> 直接 `cmake -S . -B build` 不会重算。

### 2.2 产物

```
build/lib/libq_core.a        build/lib/libq_core.so        (libq_core.so.0 -> libq_core.so.0.3.0)
build/lib/libq_log.a         build/lib/libq_log.so
build/lib/libq_conf.a        build/lib/libq_conf.so
build/lib/libq_db.a          build/lib/libq_db.so
build/lib/libq_db_mock.a     build/lib/libq_db_mock.so
build/lib/libq_db_mysql.a    build/lib/libq_db_mysql.so   （有 connector-c 才有）
build/lib/libq_mapper.a      build/lib/libq_mapper.so
build/lib/libq_httpc.a       build/lib/libq_httpc.so
build/lib/libq_http.a        build/lib/libq_http.so
build/lib/libq_reg.a         build/lib/libq_reg.so
build/bin/user_svc  order_svc  http_demo  demo_svc  q_upstream_sync
build/bin/test_core test_log test_conf test_mapper test_reg
```

动态库带 `VERSION 0.3.0 / SOVERSION 0`，因此有
`libq_x.so -> libq_x.so.0 -> libq_x.so.0.3.0` 三级软链，符合 Linux 发行版惯例。

> llhttp 的源码（`deps/llhttp/*.c`）**直接编进 `libq_http`**，不是一个独立库。
> 它属于实现细节：使用方只链接 `-lq_http` 即可，不需要也不应该单独链 llhttp。

## 3. 安装布局

```
<prefix>/include/q/core/*.h   conf.h  db.h  db_mock.h  http.h  httpc.h  log.h  mapper.h  reg.h
<prefix>/lib/libq_*.a
<prefix>/lib/libq_*.so*
<prefix>/lib/pkgconfig/qboot.pc
<prefix>/lib/cmake/qboot/qboot-config.cmake
<prefix>/bin/q_upstream_sync
<prefix>/share/qboot/deploy/nginx/*.conf
```

装到非标准目录（如 `/opt/qboot`）时，运行期要告知动态链接器：

```bash
echo /opt/qboot/lib | sudo tee /etc/ld.so.conf.d/qboot.conf && sudo ldconfig
# 或临时：
export LD_LIBRARY_PATH=/opt/qboot/lib
```

## 4. 在自己的服务里用 qboot

### 4.1 pkg-config（最简单）

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
cc -o my_svc my_svc.c $(pkg-config --cflags --libs qboot)
```

`templates/minimal_svc/Makefile` 已经是这个写法，并且区分了静态/动态：

```bash
cd templates/minimal_svc
make          # 静态：把 .a 链进去，产物自包含
make shared   # 动态：产物小，运行环境要有 .so
```

> 静态链接必须用 `.a` 的**全路径**。只写 `-lq_http` 时 ld 会优先挑同目录的 `.so`，
> 所谓"静态"就名不副实了。`make` 里用 `pkg-config --variable=libdir` 拼出全路径，
> 并用 `filter-out -lq_%` 去掉 `pkg-config --static --libs` 里 qboot 自己的 `-l`。

### 4.2 CMake find_package

```cmake
find_package(qboot REQUIRED)
target_link_libraries(my_svc PRIVATE
    qboot::q_http qboot::q_mapper qboot::q_reg qboot::q_db qboot::q_core)
```

```bash
cmake -S . -B build -Dqboot_DIR=/usr/local/lib/cmake/qboot
```

每个库都有两个目标：

| 目标 | 链接 |
|---|---|
| `qboot::q_core` | `libq_core.a` |
| `qboot::q_core_shared` | `libq_core.so` |

要整套换动态库就给每个名字加 `_shared`，
`templates/minimal_svc/CMakeLists.txt` 里用 `-DQBOOT_USE_SHARED=ON` 一把切。

### 4.3 直接用 -l

```bash
cc -o my_svc my_svc.c -I/usr/local/include \
   -L/usr/local/lib \
   -lq_http -lq_mapper -lq_reg -lq_httpc -lq_db_mysql -lq_db_mock -lq_db \
   -lq_conf -lq_log -lq_core \
   -levent -levent_pthreads -lcurl -lxml2 -ljansson -lmariadb -lpthread
```

顺序是**依赖者在前、被依赖者在后**，GNU ld 对静态库顺序敏感，别乱排。

## 5. 常见构建问题

| 现象 | 原因 | 修复 |
|---|---|---|
| `libevent not found -> skip libq_http` | 没装 libevent | `sudo apt install libevent-dev` |
| 链接报 `undefined reference to llhttp_*` | 用了很旧的中间产物 | `rm -rf build && ./scripts/build.sh` |
| `ld: library 'mariadb' not found` | connector-c 装在非标准路径 | `qboot.pc` 的 `Libs.private` 里已带 `-L`；仍不行就 `export LDFLAGS=-L<路径>` |
| `pkg-config 找不到 qboot` | PKG_CONFIG_PATH 没设 | `export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig` |
| `find_package(qboot) 失败` | 没传 qboot_DIR | `-Dqboot_DIR=<prefix>/lib/cmake/qboot` |
| 运行时 `libq_core.so.0: cannot open shared object file` | ld 缓存没刷新 | `sudo ldconfig` |
| 改了 CMakeLists 默认值没生效 | `option()` 缓存 | 显式传 `-Dxxx=ON/OFF`，或删掉 `build/CMakeCache.txt` |

## 6. 交叉注意

- **Windows 不支持**，只做 Linux（含麒麟 / 统信）和 macOS（调试用）。
- 静态库全部带 `-fPIC`（`CMAKE_POSITION_INDEPENDENT_CODE ON`），
  否则它们链不进 `.so`。
- 动态库的 `RPATH` 会写入安装前缀的 `lib`，所以装到别处后
  二进制仍能在同目录找到兄弟库。
