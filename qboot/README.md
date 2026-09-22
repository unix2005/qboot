# qboot

C 语言微服务框架：nginx 接入 + XML 外置 SQL + Consul 注册发现 + 模块化库。
命名约定：库 `libq_xxx`，函数/类型 `q_`，宏/枚举 `Q_`，头文件 `q/xxx.h`。

新机器上三步走：

```bash
sudo ./scripts/install_deps_ubuntu.sh     # 装依赖（只做一次）
./scripts/build.sh                        # 编译 + 安装到 /usr/local（.a 和 .so 都产出）
./scripts/test_all.sh                     # 137 项断言，应全过
```

## 构建

```bash
cmake -S . -B build -DQ_BUILD_HTTP=ON
cmake --build build -j8
sudo cmake --install build
```

每个库同时产出静态库和动态库：

| 静态 | 动态 |
|---|---|
| `libq_core.a` | `libq_core.so`（`-> .so.0 -> .so.0.3.0`） |
| …… | …… |

可选开关：`-DQ_BUILD_SHARED=OFF`（只出 `.a`）、`-DQ_ENABLE_ASAN=ON` /
`-DQ_ENABLE_TSAN=ON` / `-DQ_ENABLE_UBSAN=ON`、`-DQ_BUILD_SAMPLES=OFF` 等。
`option()` 有缓存，改默认值必须显式传 `-D`。

依赖缺失时对应模块会自动跳过，不会让构建失败：

| 依赖 | 缺失时跳过 |
|---|---|
| libevent | libq_http |
| libxml2 / jansson | libq_mapper |
| libcurl | libq_httpc、libq_reg |
| MariaDB Connector/C | libq_db_mysql |

## 跑测试

```bash
./scripts/test_all.sh                          # 一次跑完：core 44 / conf 12 / log 4 / mapper 49 / reg 28
./scripts/test_all.sh --asan                   # ASan 版本
./scripts/test_all.sh --verbose --filter mapper
```

单独跑也行：

```bash
./build/bin/test_core                          # 44 项
./build/bin/test_conf                          # 12 项
./build/bin/test_mapper                        # 49 项
./build/bin/test_reg                           # 28 项（内置假 Consul，不用装 Consul）
```

## 跑样例服务

```bash
./scripts/run_demo.sh                          # 一键起 user-svc + order-svc，打一遍接口
./scripts/run_demo.sh --keep                   # 跑完不退出，方便自己 curl

./build/bin/user_svc  samples/conf/user_svc.ini
./build/bin/order_svc samples/conf/order_svc.ini
```

| 服务 | 端口 | 演示什么 |
|---|---|---|
| minimal_svc | 9090 | 最小骨架（`templates/minimal_svc`） |
| user-svc | 8081 | HTTP + XML mapper + DB + Consul 注册 |
| order-svc | 8082 | 服务间调用（order-svc → user-svc，只认服务名） |

默认连的是 `mock://` 内存假驱动，**没有 MySQL 也能起来**。接口：

```bash
curl 'http://127.0.0.1:8081/user/1'
curl 'http://127.0.0.1:8081/user/search?name=a%25&minAge=18'
curl -XPOST http://127.0.0.1:8081/user -d '{"name":"lisi","age":20,"balance":1.5,"status":1}'
curl -XPUT   http://127.0.0.1:8081/user/5 -d '{"name":"ww","age":33}'
curl -XDELETE http://127.0.0.1:8081/user/5
```

换成真库：改 `samples/conf/user_svc.ini` 里的 `[db] url` 为
`mysql://user:pass@host:3306/db`，**代码一行都不用改**。

## 模块

| 库 | 目录 | 说明 |
|---|---|---|
| libq_core | core/ | 内存池、strbuf、数组、哈希表、时间、线程池、错误码 |
| libq_log | log/ | 环形队列 + 后台线程批量写 + 轮转 + ThreadLocal trace 上下文 |
| libq_conf | conf/ | ini + 环境变量覆盖（`Q_<SEC>_<KEY>`） |
| libq_http | http/ | 入站 HTTP：libevent + llhttp，SO_REUSEPORT 多线程 |
| libq_httpc | httpc/ | 出站 HTTP：libcurl 薄封装，每线程一个 handle |
| libq_db | db/ | 统一 DB 抽象（ops vtable + dialect）+ 连接池 + 事务 |
| libq_db_mysql | db/driver/mysql/ | MariaDB Connector/C 驱动 |
| libq_db_mock | db/driver/mock/ | 内存假驱动，无库环境的自测/演示底座 |
| libq_mapper | mapper/ | XML 外置 SQL + 动态标签 + 结果映射 |
| libq_reg | registry/ | Consul 注册 / TTL 心跳 / 发现 / 轮询 / 服务间调用 |

## 最小服务骨架

```c
q_log_init("logs", "my-svc", Q_LOG_INFO);

q_db_register_mock();
#ifdef Q_HAVE_MYSQL
q_db_register_mysql();
#endif
q_dbp_t   *pool   = q_dbp_new("mysql://u:p@127.0.0.1:3306/db", 8, 60, 4);
q_mapper_t *mapper = q_mapper_load("mapper");

q_http_t *h = q_http_new(8081, 4);
q_http_route_health(h);
q_http_route(h, "GET", "/user/:id", handler, NULL);
q_http_run(h);        /* 阻塞，SIGTERM 时 q_http_stop(h) 退出 */
```

## 部署（nginx + Consul）

```bash
consul agent -dev &
./user_svc samples/conf/user_svc.ini &        # 自动注册 + TTL 心跳

./build/tools/q_upstream_sync 127.0.0.1:8500 /usr/local/etc/nginx/upstream \
    --reload-cmd "nginx -s reload" user-svc order-svc &

nginx -c deploy/nginx/nginx.conf
```

`q_upstream_sync` 等价 consul-template 但零额外依赖：拉 Consul 生成
`upstream/<service>.conf`，内容变了才写盘并 reload。配置样例见 `deploy/nginx/`。

## 写自己的服务

装完之后两种接入方式：

```bash
# pkg-config
cc -o my_svc my_svc.c $(PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --cflags --libs qboot)

# CMake
find_package(qboot REQUIRED)
target_link_libraries(my_svc PRIVATE qboot::q_http qboot::q_mapper qboot::q_core)
# cmake -S . -B build -Dqboot_DIR=/usr/local/lib/cmake/qboot
```

每个库都有 `<name>` 和 `<name>_shared` 两个目标，前者链 `.a`，后者链 `.so`。
可复制的骨架在 `templates/minimal_svc/`。

## 文档

- **`docs/BUILD.md`** —— 构建、安装、接入（pkg-config / find_package / 直接 -l）
- **`docs/USAGE.md`** —— 写服务：配置、日志、路由、mapper XML、注册发现、部署
- **`docs/TESTING.md`** —— 本机已验证清单 + Ubuntu 上逐步测试命令与预期输出
- `docs/architecture-v1.md` —— 总体方案
- `docs/logging-design.md` —— 日志设计（为什么不用 zlog）
- `docs/M0-progress.md` —— 底座
- `docs/M2-progress.md` —— XML Mapper
- `docs/M3-progress.md` —— 注册发现 + nginx
- `docs/M4-progress.md` —— 工程化：.a/.so 双产物 + install/export + demo + 脚本

## 脚本

| 脚本 | 用途 |
|---|---|
| `install_deps_ubuntu.sh` | 装依赖（可选 `--with-consul` / `--with-mysql-server` / `--with-nginx`） |
| `build.sh` | 编译 + 安装 |
| `test_all.sh` | 跑全部单元测试 |
| `run_demo.sh` | 一键起 demo 跑端到端 |
| `smoke_mysql.sh` | 真库端到端（配 `sql/qboot_demo.sql`） |
| `smoke_consul.sh` | Consul 注册发现端到端 |
