# qboot

C 语言微服务框架：nginx 接入 + XML 外置 SQL + Consul 注册发现 + 模块化静态库。
命名约定：库 `libq_xxx`，函数/类型 `q_`，宏/枚举 `Q_`，头文件 `q/xxx.h`。

## 构建

```bash
cmake -S . -B build -DQ_BUILD_HTTP=ON
cmake --build build -j8
```

可选开关：`-DQ_ENABLE_ASAN=ON` / `-DQ_ENABLE_TSAN=ON` / `-DQ_ENABLE_UBSAN=ON`。
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
./build/tests/test_core                        # 44 项
./build/tests/test_conf                        # 12 项
./build/tests/test_mapper tests/mapper/User.xml # 49 项
./build/tests/test_reg                          # 28 项（内置假 Consul，不用装 Consul）
```

## 跑样例服务

```bash
./build/samples/user_svc samples/conf/user_svc.ini
```

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

## 文档

- `docs/architecture-v1.md` —— 总体方案
- `docs/logging-design.md` —— 日志设计（为什么不用 zlog）
- `docs/M0-progress.md` —— 底座
- `docs/M2-progress.md` —— XML Mapper
- `docs/M3-progress.md` —— 注册发现 + nginx
