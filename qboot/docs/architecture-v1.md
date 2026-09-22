# qboot — C 语言微服务框架总体设计方案 v0.2

> 定位：用 C 写一套"类 Spring Boot"的微服务开发框架，微服务实例只写业务 handler，其余能力全部由框架库提供。
> 一期范围：HTTP 接入 + XML 外置 SQL + MySQL 驱动 + Consul 服务注册发现 + nginx 网关接入。
> Oracle / 达梦：本期只做抽象层与方言接口预留，驱动后补。

## 命名约定

| 类别 | 前缀 / 形式 | 示例 |
|---|---|---|
| 框架代号 | `qboot` | 仓库根目录 `qboot/` |
| 库 | `libq_<模块>` | `libq_core.a`、`libq_db.a` |
| 函数 / 类型 | `q_` | `q_app_new()`、`q_conn_t` |
| 宏 / 枚举 | `Q_` | `Q_ROUTE_GET`、`Q_T_INT64` |
| 头文件 | `q/<模块>.h` | `#include <q/boot.h>` |

## 已确认决策（v0.2）

| 项 | 结论 | 影响 |
|---|---|---|
| 进程模型 | **单进程多线程** | 无需共享内存；但一个线程崩溃会导致整个进程退出，靠内存池 + 信号兜底 + 自动重启缓解 |
| MySQL 客户端 | **MariaDB Connector/C (LGPL)** | API 与 libmysqlclient 兼容，闭源交付无 GPL 风险 |
| 日志 | **不用 zlog，自研 `libq_log`**，沿用现有 `q_log` 的接口风格重写实现 | 详见 `docs/logging-design.md` |
| 操作系统 | **Linux（含麒麟/统信）+ macOS 开发调试，不做 Windows** | IO 复用用 libevent 封装 epoll/kqueue，不引入 IOCP 分支 |
| 数据库 | 一期 MySQL；Oracle / 达梦走驱动抽象与方言，后续接 | DB 接口必须一次设计到位 |

---

## 1. 目标与边界

**要做**

- 一个 `main.c` + 一个 `mapper.xml` + 若干 handler 就能跑起一个可注册的微服务
- SQL 全部外置在 XML，改 SQL 不改 C 代码、不重编译
- 统一数据库访问接口，一期 MySQL，后续 Oracle / 达梦通过驱动插件接入
- 服务启动自动注册到注册中心，nginx 自动感知实例上下线
- 线程模型、连接池、日志、配置、健康检查、优雅退出由框架兜底

**本期不做**

- 私有 RPC 协议（一期服务间调用走 HTTP + JSON）
- 分布式事务、分库分表中间件
- ORM 全自动关联映射（C 无反射，做到"结果集 → struct / JSON"的半自动映射为止）

---

## 2. 总体架构

```
                 ┌──────────────┐
   客户端 ──────▶ │    nginx     │  接入层：反向代理 / 负载均衡 / TLS / 限流
                 └──────┬───────┘
          consul-template 动态渲染 upstream.conf → nginx -s reload
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
   ┌─────────┐    ┌─────────┐    ┌─────────┐
   │ svc A   │    │ svc A   │    │ svc B   │   多实例，各自注册到 Consul
   │ :8081   │    │ :8082   │    │ :8083   │
   └────┬────┘    └────┬────┘    └────┬────┘
        └───────────────┼───────────────┘
                        │ register / health check / deregister
                 ┌──────▼───────┐
                 │   Consul     │
                 └──────────────┘
                        │
                 ┌──────▼───────┐
                 │ MySQL / DM / │
                 │ Oracle       │
                 └──────────────┘
```

### 单实例内部：单进程多线程

```
进程（单个）
 ├─ 主线程          listen / accept（SO_REUSEPORT 时各 IO 线程自行 accept）
 ├─ IO 线程 ×N      每线程一个 event_base(epoll/kqueue)：连接读写、HTTP 解析
 ├─ 业务线程池 ×M   执行 handler 与 SQL，慢查询不阻塞 IO 线程
 └─ 后台线程        连接池保活回收、Consul 续约、配置热拉取、日志异步刷盘
```

**线程协作规则（决定要不要加锁）**

| 数据 | 归属 | 并发策略 |
|---|---|---|
| 监听 socket | 全部 IO 线程共享 | `SO_REUSEPORT` 各线程独立 accept，内核分发，省掉跨线程传 fd |
| 连接对象、请求内存池 | 绑定到某个 IO 线程 | 连接生命周期内不跨线程，**无锁** |
| 路由表、mapper AST、配置快照 | 全局 | 启动期构建完毕，运行期只读，**无锁**；热更新用原子换指针 |
| DB 连接 | ThreadLocal 专用连接（事务）+ 全局池（非事务） | 全局池用分段锁，按线程 id 分片降低竞争 |
| 语句缓存（prepared stmt） | 每连接一份 | 不跨线程共享，`MYSQL_STMT` 本身非线程安全 |
| 统计指标（QPS、连接池水位） | 全局 | 原子变量 / 分片计数器，避免热点锁 |
| 日志 | 全局 | 无锁环形队列 + 后台线程刷盘 |

**单进程多线程的风险与对策**（这条要写在最前面，别等出事再补）

1. 任一线程段错误 = 整个进程退出。对策：所有请求内存从 request pool 分配、请求结束整体释放；`SIGSEGV` 捕获后 `backtrace_symbols()` 打栈并生成 core；部署侧用 systemd `Restart=always` 或容器重启策略。
2. 启动即注册、崩溃来不及反注册 → Consul 侧靠健康检查（10s 间隔、连续 2 次失败即摘除）兜底，最坏 20s 内剔除。
3. 编译期开 `-Wall -Wextra -Werror`，CI 强制跑 ASan / TSan / valgrind 三套，把多线程问题拦在上线前。

---

## 3. 技术选型

| 能力 | 选型 | 理由 | 备选 |
|---|---|---|---|
| 事件循环 / IO | **libevent** (BSD) | 封装 epoll/kqueue；需开 `EVTHREAD_USE_PTHREADS` 支持多线程 | libuv |
| HTTP 解析 | **llhttp** (MIT) | Node.js 同款，C99、零分配、性能极高，只做解析不管 IO，可控性最强 | 无必要备选 |
| HTTP Server 全栈 | （备选）**civetweb** (MIT) | 打样最快，自带路由/静态文件/WebSocket | libmicrohttpd (LGPL) |
| 路由匹配 | **pcre2** (BSD) | `:id` 风格路径参数、通配 | 自写前缀树 |
| JSON | **jansson** (MIT) | API 干净、引用计数、可读写 | cJSON（更轻）、yyjson（只读更快） |
| XML | **libxml2** (MIT) | 启动期用 xmlTextReader 流式解析 mapper，XPath 便于 include/sql 片段处理 | expat（更轻） |
| MySQL 驱动 | **MariaDB Connector/C** (LGPL) | API 与 libmysqlclient 兼容，协议为 LGPL，闭源交付无 GPL 风险 | libmysqlclient |
| Oracle（后期） | **ODPI-C** (Apache-2.0) | Oracle 官方 C 封装，比裸 OCI 好用 | OCI |
| 达梦（后期） | **DM DPI** | 达梦自带，API 风格接近 OCI，抽象层好统一 | 统一 ODBC 通道 |
| 日志 | **自研 `libq_log`** | 沿用现有 `q_log` 接口风格，实现换成异步刷盘 + 级别 + 分类 + trace 上下文；零依赖，约 300~500 行。详见 `docs/logging-design.md` | 无 |
| HTTP Client | **libcurl** (MIT-like) | 调 Consul API、服务间调用、配置拉取 | 自写（基于 libevent） |
| 配置 | **inih** (BSD) 起步，后续 libcyaml | ini 足够小，YAML 可读性更好 | JSON 配置 |
| 构建 | **CMake** + **vcpkg/conan** | 跨平台，依赖版本可锁定；麒麟/统信可用系统包 + FetchContent | Makefile |
| 注册中心 | **Consul** | HTTP API 极简（curl 即可）、自带健康检查、DNS 接口便于 nginx 集成、KV 可做配置中心 | Nacos（接口抽象后随时可换） |
| 单元测试 | **Unity / cmocka** + CTest | CI 里跑 ASan/valgrind | — |

目标平台：**Linux（x86_64 / aarch64，含麒麟、统信）+ macOS（仅开发调试）**。不引入任何 Windows 分支代码。

**不用 zlog 的原因**：维护已停滞、配置自成一套，最关键的是不支持 MDC 式上下文字段——框架要透传 `trace_id` / `request_id`，用 zlog 只能在每个日志点手动拼字符串。而日志库本身代码量很小，自研反而更可控。

---

## 4. 模块化库划分

每个模块产出独立静态库（同时可编成 `.so`），微服务按需链接。

| 库 | 职责 | 依赖第三方 |
|---|---|---|
| `libq_core` | 内存池/arena、字符串、动态数组、哈希表、链表、错误码、时间、UUID、线程池、原子统计 | — |
| `libq_log` | 分类日志、级别过滤、异步刷盘、按大小/日期轮转、trace 上下文 | — |
| `libq_conf` | 配置文件（ini/yaml）、环境变量覆盖、热加载 | inih |
| `libq_json` | JSON 读写封装、json ↔ 参数上下文转换 | jansson |
| `libq_http` | 事件循环封装、HTTP server（llhttp）、router、中间件链、HTTP client | libevent, llhttp, pcre2, libcurl |
| `libq_db` | 数据源抽象（vtable 驱动接口）、连接池、预编译语句、事务、方言 | MariaDB Connector/C |
| `libq_mapper` | XML SQL 解析、动态 SQL 引擎、参数绑定、结果集 → JSON/struct 映射、语句缓存 | libxml2, libq_db, libq_json |
| `libq_registry` | 服务注册/反注册、健康检查端点、服务发现 + 本地缓存 + 负载均衡 | libcurl, libq_core |
| `libq_boot` | 启动器：串起配置→日志→mapper→db→registry→http，信号处理、优雅退出、线程编排 | 以上全部 |
| `libq_ext` | 限流、熔断、JWT 校验、Prometheus metrics、trace 透传 | 可选 |

目录结构：

```
qboot/
├── CMakeLists.txt
├── cmake/                  # Find*.cmake、编译选项（ASan/TSan 开关）
├── deps/                   # vcpkg.json / conanfile.txt，或 third_party submodule
├── core/                   # → libq_core
├── log/  conf/  json/
├── http/                   # → libq_http
├── db/
│   ├── include/q/db.h      # 抽象接口（稳定，不随驱动变动）
│   ├── driver/mysql/       # → libq_db_mysql（本期）
│   ├── driver/oracle/      # 后期
│   └── driver/dameng/      # 后期
├── mapper/                 # → libq_mapper
├── registry/               # → libq_registry（consul / nacos 适配器）
├── boot/                   # → libq_boot
├── tools/
│   └── gen_meta.py         # 从 struct 头文件生成字段元数据（解决 C 无反射）
├── samples/
│   └── user-svc/
│       ├── main.c
│       └── conf/
│           ├── app.ini
│           └── mapper/user.mapper.xml
└── tests/
```

依赖方向严格单向：`core ← log/conf/json ← http/db ← mapper/registry ← boot`，禁止反向依赖和循环依赖。

---

## 5. HTTP 层设计

请求生命周期：

```
accept（IO 线程）
  → 连接对象（分配自连接内存池，绑定本线程）
  → llhttp 解析（on_url / on_header / on_body 回调）
  → 路由匹配 → 提取路径参数、query、body(JSON)
  → 中间件链：访问日志 → 鉴权 → 限流 → trace 注入
  → 投递到业务线程池执行 handler（IO 密集的小请求也可直接在 IO 线程跑，由配置决定）
  → 响应序列化（JSON）→ 写回（仍由原 IO 线程 event_base 负责）
  → 连接关闭 / keep-alive 复用，重置请求内存池
```

跨线程交接用 libevent 的 `event_base_once` + 管道唤醒，禁止业务线程直接写 socket。

Handler 签名统一：

```c
typedef int (*q_handler_t)(q_req_t *req, q_resp_t *resp, void *ud);
```

路由注册宏（贴近 Spring 的注解体验）：

```c
Q_ROUTE_GET (app, "/api/users/:id", user_get);
Q_ROUTE_POST(app, "/api/users",     user_create);
```

`libq_http` 内置 `/health`、`/metrics`、`/info` 三个端点，无需业务编写。

---

## 6. XML SQL Mapper 设计（核心）

借鉴 MyBatis，但裁剪到 C 能优雅实现的子集。

**示例 `conf/mapper/user.mapper.xml`**

```xml
<mapper namespace="user">
  <sql id="cols">id,name,age,status,create_time</sql>

  <select id="list" resultType="json">
    SELECT <include refid="cols"/> FROM t_user
    <where>
      <if test="name != null and name != ''">
        AND name LIKE CONCAT('%', #{name}, '%')
      </if>
      <if test="minAge != null">AND age &gt;= #{minAge}</if>
      <if test="status != null">AND status = #{status}</if>
    </where>
    ORDER BY id DESC
    LIMIT #{offset}, #{limit}
  </select>

  <insert id="insert" useGeneratedKeys="true" keyProperty="id">
    INSERT INTO t_user(name, age, status) VALUES(#{name}, #{age}, #{status})
  </insert>

  <update id="updateStatus">
    UPDATE t_user SET status = #{status} WHERE id = #{id}
  </update>
</mapper>
```

**支持的动态标签（一期）**：`select/insert/update/delete`、`if`、`choose/when/otherwise`、`where`、`set`、`foreach`、`sql/include`、`${}` 拼接与 `#{}` 绑定。
`${}` 只用于表名/排序字段等无法绑定的位置，框架强制做白名单校验或标识符转义，防注入。

**执行流程**

1. 启动期：主线程扫描 `mapper/*.xml` → xmlTextReader 解析为内存 AST（`namespace.id` → 模板树），**运行期零 XML 解析开销**，且只读无锁
2. 运行期：`q_mapper_select(ctx, "user.list", params_json)` →
3. 用参数上下文求值动态标签 → 生成最终 SQL 文本 + 有序参数数组
4. 以 `namespace.id + 分支签名` 为 key 查**本连接的预编译语句缓存**（命中复用 `MYSQL_STMT`，未命中则 prepare）
5. 按类型绑定参数 → 执行 → 结果集转换 → 返回
6. 若开启事务，语句固定走线程的 ThreadLocal 连接

**结果映射（C 无反射的处理办法）**

- 默认路径：结果集 → `json_t` 数组（零配置，写接口最舒服）
- 结构体路径：用字段元数据表描述映射，由 `tools/gen_meta.py` 从头文件自动生成，避免手写：

```c
typedef struct { int64_t id; char name[64]; int age; } user_t;

static const q_field_t USER_FIELDS[] = {          /* 由 gen_meta.py 生成 */
    Q_FIELD(user_t, id,   Q_T_INT64),
    Q_FIELD(user_t, name, Q_T_STRING),
    Q_FIELD(user_t, age,  Q_T_INT),
    Q_FIELD_END
};

int n = q_mapper_select_struct(ctx, "user.list", params,
                               USER_FIELDS, out_array, cap);
```

**事务**

```c
Q_TX_BEGIN(ctx) {                     /* 绑定线程专用连接 + BEGIN */
    q_mapper_update(ctx, "user.updateStatus", p1);
    q_mapper_insert(ctx, "log.insert",        p2);
} Q_TX_COMMIT(ctx);                   /* 失败自动 ROLLBACK，异常路径也回滚 */
```

---

## 7. 数据库抽象层设计

统一 C 接口（纯函数指针 vtable），驱动以插件形式注册：

```c
typedef struct q_db_driver {
    const char *name;                 /* "mysql" / "oracle" / "dameng" */
    int (*connect)(q_conn_t **out, const q_dsn_t *dsn);
    int (*prepare)(q_conn_t *, q_stmt_t **out, const char *sql);
    int (*bind)(q_stmt_t *, q_param_t *params, size_t n);
    int (*exec)(q_stmt_t *);
    int (*fetch)(q_stmt_t *, q_row_t **row);      /* 流式取行 */
    int (*begin)(q_conn_t *);  int (*commit)(q_conn_t *); int (*rollback)(q_conn_t *);
    const q_dialect_t *dialect;       /* 方言 */
} q_db_driver_t;
```

**方言（`q_dialect_t`）是关键**，因为三家差异主要在：

| 差异点 | MySQL | Oracle | 达梦 |
|---|---|---|---|
| 分页 | `LIMIT a,b` | `ROWNUM` / `OFFSET...FETCH` | 支持 `LIMIT`，也兼容 ROWNUM |
| 自增主键 | `LAST_INSERT_ID()` | 序列 + `RETURNING` | 序列 / 自增列 |
| 占位符 | `?` | `:1,:2` | `?`（OCI 风格 `:n`） |
| 类型映射 | MYSQL_TYPE_* | NUMBER/VARCHAR2/CLOB | 类 Oracle |

抽象层统一为 `?` 位置占位符，由驱动改写为各自风格。mapper 里写分页时建议提供 `<page>` 标签由方言展开，避免为不同库写多份 SQL。

**连接池（适配多线程）**

- 每个业务线程持有一条 ThreadLocal 专用连接，事务期间独占，避免加锁
- 非事务查询从全局池借还；全局池按线程 id 分段（shard）加锁，降低竞争
- 连接保活：后台线程定时 `mysql_ping`；断线自动重连并对上层透明重试一次
- 慢 SQL 日志（分类 `sql`，打印参数与耗时）、连接借出超时告警、泄漏检测（借出时记录调用栈）

---

## 8. 服务注册与 nginx 接入

**注册方式（零 SDK 路线）**

- 服务启动时由 `libq_registry` 向本机 Consul Agent 发一次 `PUT /v1/agent/service/register`（libcurl），声明 `HTTP check: GET /health, interval 10s`
- 健康检查由 Consul 主动打 `/health`，业务侧无需跑定时任务
- 收到 SIGTERM → 先 deregister → 停止 accept → 等待进行中请求完成（超时强杀）→ 退出，实现优雅下线

**nginx 感知实例变化，三选一**

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| A. consul-template（推荐起步） | 监听 Consul，渲染 `upstream.conf`，触发 `nginx -s reload` | 零 Lua，运维简单，社区成熟 | reload 有秒级延迟，长连接会抖动 |
| B. OpenResty + lua-resty-consul | `balancer_by_lua` 动态选后端 | 实时生效、无 reload、可做权重/灰度 | 需要 Lua 开发能力 |
| C. nginx resolver + Consul DNS | `resolver 127.0.0.1:8600;` + 变量形式 `proxy_pass http://$svc` | 最轻，改动小 | 开源版对 SRV 权重支持弱，缓存行为需调优 |

建议：**A 起步，稳定后过渡到 B**。

注意一个坑：nginx 开源版 `upstream` 里写域名默认只在启动时解析一次（除非用变量形式的 `proxy_pass` 配合 `resolver`），所以方案 C 必须按变量写法来，否则实例变更不会生效。

**服务间调用**：`libq_registry` 提供 `q_call("user-service", "/api/users/1", ...)`，内部做：服务名 → 实例列表（Consul 查询 + 本地缓存 5s + 后台异步刷新）→ 负载均衡（轮询/随机/最少连接）→ HTTP 调用 → 超时/重试（熔断后期补）。调用链通过 `traceparent` 头透传。

---

## 9. 配置、日志与可观测

- 配置：`conf/app.ini`（server 端口、`io_threads`、`worker_threads`、db DSN、pool、consul、log、mapper 路径），支持环境变量覆盖（`Q_SERVER_PORT`），后期支持 Consul KV 覆盖
- 日志：自研 `libq_log`，分类 `app` / `access` / `sql` / `registry`；级别过滤 + 异步刷盘 + 按大小/日期轮转 + trace 上下文。设计详见 `docs/logging-design.md`
- 指标：`/metrics` 暴露 Prometheus 文本格式（QPS、延迟分桶、连接池水位、活跃事务数、线程池队列长度）
- 链路：透传 W3C `traceparent`，日志里打印 trace_id
- 调试：编译选项一键开 ASan/UBSan/TSan；每请求一个内存池，请求结束整体释放

---

## 10. 开发者体验

最小服务的 `main.c`：

```c
#include <q/boot.h>

static int user_get(q_req_t *req, q_resp_t *resp, void *ud) {
    q_json_t *p = q_json_new();
    q_json_set_str(p, "id", q_req_path(req, "id"));
    q_json_t *rows = NULL;
    q_mapper_select(req->ctx, "user.list", p, &rows);
    return q_resp_json(resp, 200, rows ? rows : q_json_null());
}

int main(int argc, char **argv) {
    q_app_t *app = q_app_new("user-service");
    q_app_conf(app, "conf/app.ini");
    q_app_mappers(app, "conf/mapper/*.xml");
    q_app_datasource(app, "mysql://root:pwd@127.0.0.1:3306/demo?pool=8");
    q_app_registry(app, Q_REG_CONSUL, "http://127.0.0.1:8500");
    Q_ROUTE_GET(app, "/api/users/:id", user_get);
    return q_app_run(app, argc, argv);   /* 阻塞：拉起 IO 线程与业务线程池、注册、等信号 */
}
```

---

## 11. 里程碑（按 1 名 C 工程师全职粗估）

| 阶段 | 内容 | 周期 |
|---|---|---|
| M0 脚手架 | CMake + vcpkg 依赖、`libq_core`、**`libq_log` 改造**、conf/json、多线程 HTTP server（SO_REUSEPORT + 每线程 event_base）、路由、JSON 响应、`/health`、信号处理与优雅退出 | 2 周 |
| M1 数据库 | `libq_db` 抽象 + MySQL 驱动 + 预编译 + 连接池（ThreadLocal + 分段池）+ 事务 + 慢 SQL | 2–3 周 |
| M2 Mapper | XML 解析 AST、动态标签引擎、`#{}`/`${}`、语句缓存、结果 → JSON/struct、`gen_meta.py` 代码生成器 | 3 周 |
| M3 服务治理 | Consul 注册/发现/优雅下线、nginx + consul-template、服务间调用 + 负载均衡 | 2 周 |
| M4 加固 | 限流、trace、metrics、单元测试 + ASan/TSan CI、示例服务与文档 | 2 周 |
| M5 多库 | Oracle(ODPI-C)、达梦(DPI) 驱动 + 方言适配 | 各 2–3 周 |

---

## 12. 剩余待定项

1. **动态标签范围**：建议一期只做 `if/choose/where/set/foreach/include`，不追求 MyBatis 全量，否则 XML 引擎会失控。
2. **业务是否投递到独立线程池**：小请求在 IO 线程直接跑（少一次线程切换），慢 SQL 才投递。建议默认直接在 IO 线程跑，需要时显式标记 `Q_ROUTE_SLOW`。
3. **配置格式**：ini 起步够用，是否直接上 YAML（可读性好但要引 libcyaml）。
4. **注册中心接口抽象**：建议做（成本约几百行），保留 Nacos 适配位。
5. **struct 元数据生成**：建议 M2 一开始就把 `gen_meta.py` 写了，别等人手写映射表。
