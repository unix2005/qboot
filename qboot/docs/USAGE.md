# qboot 使用手册

面向"我要用 qboot 写一个微服务"的人。读这一篇 + `samples/` 和 `templates/` 就够了。

命名约定：库 `libq_xxx`、函数/类型 `q_` 前缀、宏/枚举 `Q_` 前缀、头文件 `<q/xxx.h>`。

## 1. 十分钟写第一个服务

### 1.1 复制模板

模板随安装一起发布（`share/qboot/templates/`），从源码树拿也一样：

```bash
cp -r /usr/local/share/qboot/templates/minimal_svc my_svc
cd my_svc
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
make && ./minimal_svc app.ini
```

模板本体就一个文件 `minimal_svc.c`（60 行），做了四件事：读配置、起日志、
挂路由、跑事件循环。

### 1.2 骨架长什么样

```c
#include <q/conf.h>
#include <q/http.h>
#include <q/log.h>

static int h_hello(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)req; (void)ud;
    return q_resp_json(resp, 200, "{\"msg\":\"hello\"}");
}

int main(void)
{
    q_conf_t *cf = q_conf_load("app.ini");
    q_log_init(q_conf_get(cf, "log", "dir", "logs"), "my-svc", Q_LOG_INFO);

    q_http_t *h = q_http_new(q_conf_get_int(cf, "server", "port", 9090), 4);
    q_http_route_health(h);                       /* 内置 /health */
    q_http_route(h, "GET", "/hello", h_hello, NULL);
    q_http_run(h);                                /* 阻塞，直到 q_http_stop */
    q_http_free(h); q_conf_free(cf); q_log_close();
}
```

优雅退出：`signal(SIGTERM, on_signal)`，在 handler 里调 `q_http_stop(h)` 即可，
`q_http_run` 会返回，后面的清理代码正常执行。

## 2. 配置

ini 格式，`;` 开头是注释：

```ini
[server]
name       = my-svc
port       = 9090
io_threads = 4

[db]
url  = mysql://root:123456@127.0.0.1:3306/demo
pool = 8
```

```c
q_conf_t   *cf   = q_conf_load("app.ini");
const char *name = q_conf_get(cf, "server", "name", "my-svc");   /* 取不到用默认值 */
int         port = q_conf_get_int(cf, "server", "port", 9090);
int         on   = q_conf_get_bool(cf, "consul", "enable", 0);
```

**环境变量优先于文件**：`Q_<SEC>_<KEY>`，section/key 转大写。
比如 `Q_SERVER_PORT=9999 ./my_svc` 会覆盖文件里的 `port`。
容器里改端口不用改配置文件。

> 配置文件不存在时 `q_conf_load` 返回 NULL，代码里有默认值兜底，服务照样能起来。

## 3. 日志

```c
q_log_init("logs", "my-svc", Q_LOG_INFO);     /* dir, app 名, 级别 */
q_log_add_cat("sql", 64*1024*1024, 7);        /* 独立分类：64MB 轮转，留 7 天 */

q_info("started on %d", port);
q_error("db down: %s", err);
q_info_cat("sql", "SQL: %s  %ldus", sql, us); /* 写到 sql 分类文件 */

q_log_set_ctx(trace_id, req_id);              /* ThreadLocal，之后每条日志自动带上 */
q_log_clear_ctx();

q_log_close();                                /* 退出前调用，保证落盘 */
```

级别：trace < debug < info < warn < error < fatal。
配置文件里 `[log] level = info` 配合 `q_log_level_from_str()` 使用。

实现：环形队列 + 后台线程批量写，业务线程不碰磁盘。
`q_log_sync_flush()` 只调 `write(2)`，**可以在信号处理器里用**。

## 4. HTTP

### 4.1 路由

```c
q_http_route(h, "GET",    "/user/:id",     h_get,    NULL);
q_http_route(h, "POST",   "/user",         h_add,    NULL);
q_http_route(h, "PUT",    "/user/:id",     h_update, NULL);
q_http_route(h, "DELETE", "/user/:id",     h_del,    NULL);
```

> **路由顺序有讲究**：按下标顺序匹配，`/user/search` 必须排在 `/user/:id` 前面，
> 否则 `search` 会被 `:id` 吃掉。

### 4.2 取请求 / 写响应

```c
const char *id      = q_req_param(req, "id");          /* /user/:id */
const char *keyword = q_req_query(req, "name");        /* ?name=xxx */
const char *ct      = q_req_header(req, "Content-Type");
const char *body    = q_req_body(req);
size_t      blen    = q_req_body_len(req);

q_resp_json(resp, 200, json_str);                       /* 自动带 Content-Type */
q_resp_text(resp, 404, "not found");
q_resp_header(resp, "X-Trace-Id", tid);
q_resp_status(resp, 204);
```

handler 返回 int，框架不关心返回值内容（返回 0 即可）。

### 4.3 线程模型

`q_http_new(port, io_threads)`：SO_REUSEPORT + 每个 IO 线程一个 `event_base`，
handler **在 IO 线程里执行**。

- `q_ctx_t`（mapper 上下文）、`q_httpc_t`（curl handle）都要**每线程一个**，
  用 `static __thread` 缓存，不要加锁共享。
- handler 里不要阻塞（不要做慢 IO），否则整个线程上的连接都卡住。

## 5. 数据库

### 5.1 连接池

```c
q_db_register_mock();          /* 内存假驱动，无库环境必开 */
#ifdef Q_HAVE_MYSQL
q_db_register_mysql();
#endif

q_dbp_t *pool = q_dbp_new("mysql://root:123456@127.0.0.1:3306/demo",
                          8,      /* max_open */
                          60,     /* idle_secs 空闲回收 */
                          4);     /* nshards，一般等于 io_threads */
```

URL 也支持 `mock://u:p@127.0.0.1:0/demo`——**换成 mock 不用改任何业务代码**，
这是本机没有数据库时做自测和演示的关键。

> 注意 `Q_HAVE_MYSQL` 是可选的：没装 MariaDB Connector/C 的机器上不要定义它，
> 也不要链接 `libq_db_mysql`。`samples/CMakeLists.txt` 里用 `if(TARGET q_db_mysql)`
> 自动处理。

### 5.2 直接用 SQL

```c
q_conn_t *c = q_dbp_get(pool, err, sizeof(err));
q_result_t *r = NULL;
q_conn_query(c, "SELECT id,name FROM t_user WHERE id=1", &r, err, sizeof(err));
while (q_result_next(r) == 1) { ... }
q_result_free(r);
q_dbp_put(c);                    /* 借了必须还 */
```

事务用 `q_dbp_tx()` 拿线程独占连接（首次自动 BEGIN），
`q_tx_commit` / `q_tx_rollback` 后自动归还。

## 6. XML Mapper（SQL 外置）

SQL 全部写在 XML 里，改 SQL 不重新编译。

```xml
<?xml version="1.0" encoding="UTF-8"?>
<mapper namespace="user">

    <sql id="Cols">id, user_name, age, balance, status, created_at</sql>

    <select id="getById">
        SELECT <include refid="Cols"/>
        FROM t_user
        WHERE id = #{id}
    </select>

    <select id="search">
        SELECT <include refid="Cols"/>
        FROM t_user
        <where>
            <if test="name != null and name != ''">AND user_name LIKE #{name}</if>
            <if test="minAge != null">AND age &gt;= #{minAge}</if>
            <if test="statusList != null">
                AND status IN
                <foreach collection="statusList" item="st" open="(" close=")" separator=",">
                    #{st}
                </foreach>
            </if>
        </where>
        ORDER BY id DESC
        LIMIT #{offset}, #{size}
    </select>

    <insert id="add">
        INSERT INTO t_user (user_name, age, balance, status)
        VALUES (#{name}, #{age}, #{balance}, #{status})
    </insert>

    <update id="updateSelective">
        UPDATE t_user
        <set>
            <if test="name != null">user_name = #{name},</if>
            <if test="age != null">age = #{age},</if>
        </set>
        WHERE id = #{id}
    </update>

    <delete id="remove">DELETE FROM t_user WHERE id = #{id}</delete>
</mapper>
```

### 6.1 支持的标签

| 标签 | 作用 |
|---|---|
| `<sql id>` + `<include refid>` | 片段复用 |
| `<if test="...">` | 条件 |
| `<choose>/<when>/<otherwise>` | 多路分支 |
| `<where>` | 自动去掉首个 AND/OR，无子句时不输出 WHERE |
| `<set>` | 自动去掉末尾逗号 |
| `<foreach collection item open close separator>` | 遍历数组 |
| `#{name}` | 参数绑定（预编译占位符） |
| `${name}` | 直接拼接（**有注入风险，只用于 ORDER BY 之类**） |

`test` 表达式支持 `and / or / not`、`== != < <= > >=`、括号、`null` 判断，
字符串用单引号或双引号。数组/对象按"非空即真"处理。

### 6.2 调用

```c
q_mapper_t *mapper = q_mapper_load("mapper");      /* 目录下所有 *.xml */
q_ctx_t    *ctx    = q_ctx_new(mapper, pool);      /* 每线程一个，不要共享 */

json_t *p = json_pack("{s:s}", "id", id);
json_t *rows = NULL;
if (q_ctx_query(ctx, "user.getById", p, &rows, err, sizeof(err)) != Q_OK) { ... }
```

语句 id 是 `namespace.id`。三个入口：

| 函数 | 用途 |
|---|---|
| `q_ctx_query(ctx, id, params, &rows, err, len)` | 查询，返回 json 数组 |
| `q_ctx_exec(ctx, id, params, &affected, &insert_id, err, len)` | 更新/删除 |
| `q_ctx_insert(ctx, id, params, &insert_id, err, len)` | 插入 |

结果直接射进结构体：

```c
typedef struct { int64_t id; char name[64]; double balance; } user_t;
static const q_field_t fields[] = {
    Q_FIELD(user_t, id,       Q_F_INT64),
    Q_FIELD(user_t, name,     Q_F_STR),
    Q_FIELD(user_t, balance,  Q_F_DOUBLE),
    Q_FIELD_END
};
user_t out[64];
int n = q_ctx_query_struct(ctx, "user.search", p, fields, out,
                           sizeof(user_t), 64, err, sizeof(err));
```

### 6.3 一个坑：JSON 引用计数

`json_t` 是引用计数的。写响应时的常见模式：

```c
static int json_ok(q_resp_t *resp, json_t *data)
{
    json_t *wrap = json_object();
    json_object_set_new(wrap, "code", json_integer(0));
    json_object_set_new(wrap, "data", data ? data : json_null());   /* 接管所有权 */
    ...
    json_decref(wrap);
}
```

`json_object_set_new` **接管**引用，所以调用 `json_ok(resp, rows)` 之后
**不能再 `json_decref(rows)`**，否则就是 use-after-free
（这个项目早期就踩过，ASan 才抓出来）。想继续用就先 `json_incref`。

## 7. 服务注册与调用（Consul）

### 7.1 注册自己

```c
const char *tags[] = { "v1" };
q_reg_cfg_t cfg = {
    .consul_addr = "127.0.0.1:8500",
    .service     = "user-svc",
    .host        = "127.0.0.1",
    .port        = 8081,
    .tags        = tags, .ntags = 1,
    .ttl_secs    = 10,        /* 心跳周期 = TTL */
};
q_httpc_global_init();
q_reg_t *r = q_reg_new(&cfg);
q_reg_start(r, err, sizeof(err));   /* 注册 + 启心跳线程 + 后台刷新 */
...
q_reg_stop(r); q_reg_free(r); q_httpc_global_cleanup();
```

用的是 Consul 的 TTL check：进程挂了没人续期，Consul 到期自动标 critical。
注册时带 `DeregisterCriticalServiceAfter`，即使 `kill -9` 也会被清掉。

### 7.2 调别人

```c
q_reg_watch(r, "user-svc");                    /* 加入后台刷新列表 */
q_endpoint_t ep;
q_reg_pick(r, "user-svc", &ep);                /* 轮询选一个健康实例 */

q_httpc_resp_t out;
q_reg_call(r, "user-svc", "/user/1", "GET", NULL, &out, err, sizeof(err));
/* out.status / out.body / out.body_len */
q_httpc_resp_free(&out);
```

**业务代码只认服务名，不认 IP:PORT。** 没装 Consul 时可以退化成配置里的直连地址，
`samples/order_svc.c` 里的 `call_user_svc()` 就是这个两段式写法：

```c
if (g_reg != NULL)  return q_reg_call(g_reg, "user-svc", path, "GET", NULL, out, err, len);
/* 否则：snprintf(url, "http://%s%s", cfg_user_addr, path); q_httpc_get(...) */
```

## 8. nginx 接入

```bash
consul agent -dev -client=0.0.0.0 &
./user_svc  samples/conf/user_svc.ini  &     # 自动注册 + 心跳
./order_svc samples/conf/order_svc.ini &

# 拉 Consul 生成 upstream 文件，变了才写盘 + reload（consul-template 的零依赖替代）
./build/bin/q_upstream_sync 127.0.0.1:8500 /etc/nginx/upstream \
    --reload-cmd "nginx -s reload" user-svc order-svc &

nginx -c deploy/nginx/nginx.conf
```

配置样例见 `deploy/nginx/`。upstream 为空时会写一条
`server 127.0.0.1:1 down;` 占位，保证 nginx 配置始终合法。

## 9. 部署建议

| 场景 | 建议 |
|---|---|
| 容器 | 静态链接（`make`），镜像里不用带 `.so`；日志挂 volume |
| 多实例 | 静态链接更省事；动态链接要记得 `ldconfig` |
| 端口 | `[server] port`，容器里用 `Q_SERVER_PORT` 覆盖 |
| 优雅退出 | 处理 SIGTERM → `q_http_stop` → 清理 → `q_log_close` |
| 健康检查 | 内置 `/health` |

## 10. 排查清单

| 现象 | 查什么 |
|---|---|
| 起不来，报 `cannot listen on N` | 端口被占；`ss -lntp \| grep N` |
| 请求 404 | 路由顺序（`/user/:id` 是否吃掉了别的路径） |
| 请求一直挂着 | handler 里阻塞了；curl 走代理了（curl 要加 `--noproxy '*'`） |
| 打不到别的服务的实例 | Consul 里是否 passing；`q_reg_watch` 加过没有 |
| SQL 语法错 | 打开 `q_ctx_debug(ctx, 1)`，看生成的 SQL |
| 日志没落盘 | `q_log_close()` 调了没；日志目录可写吗 |
