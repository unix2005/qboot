# M3 进展：服务注册发现 + nginx 接入

状态：**完成并通过自测（registry 28/28，服务全接口冒烟通过）**

---

## 1. 交付内容

| 模块 | 路径 | 说明 |
|---|---|---|
| `libq_httpc` | `httpc/` | 出站 HTTP 客户端，libcurl 薄封装 |
| `libq_reg` | `registry/` | Consul 注册 / TTL 心跳 / 发现 / 轮询 / 服务间调用 |
| `q_upstream_sync` | `tools/` | nginx 上游同步器，替代 consul-template，零额外依赖 |
| nginx 配置 | `deploy/nginx/` | 主配置 + 路由 + 说明 |
| 完整样例服务 | `samples/user_svc.c` | http + db + mapper + registry 串起来 |
| 端到端测试 | `tests/test_reg.c` | 28 项断言，用假 Consul，不需要装 Consul |

---

## 2. 服务注册

Consul 用 **TTL check**：注册时声明 TTL，本进程后台线程定期 `pass`；进程挂了没人续期，
Consul 到期自动摘除。再配 `DeregisterCriticalServiceAfter=2m`，
即使被 `kill -9` 也能自动清掉，不会留下僵尸实例。

```c
q_reg_cfg_t cfg = {
    .consul_addr = "127.0.0.1:8500",
    .service     = "user-svc",
    .host        = "10.0.0.5",      /* 多网卡机器必须显式配 */
    .port        = 8081,
    .tags        = (const char *[]){"v1"},
    .ntags       = 1,
    .ttl_secs    = 10,
};
q_reg_t *r = q_reg_new(&cfg);
q_reg_start(r, err, sizeof(err));    /* 注册 + 心跳线程 + 刷新线程 */
...
q_reg_stop(r);                       /* 退出前：停线程并主动注销 */
```

实际下发的注册报文：

```json
{"ID":"user-svc-10.0.0.5-8081","Name":"user-svc","Address":"10.0.0.5","Port":8081,
 "Tags":["v1"],
 "Check":{"TTL":"10s","Status":"passing","DeregisterCriticalServiceAfter":"2m"}}
```

## 3. 服务发现与负载均衡

- 后台线程按 `refresh_secs` 拉 `GET /v1/health/service/<name>?passing=1`
- 结果缓存在本地哈希表，`q_reg_pick()` 走轮询，**调用路径上不打 Consul**
- Consul 挂了：缓存保留最后一次成功的结果，服务继续可用（只是列表不再更新）
- `q_reg_call()` 一次调用最多试 3 个实例，失败自动换下一个

```c
q_endpoint_t ep;
q_reg_pick(r, "order-svc", &ep);

q_httpc_resp_t out;
q_reg_call(r, "order-svc", "/order/list", "GET", NULL, &out, err, sizeof(err));
```

## 4. nginx 接入

```
            客户端
              │
          nginx (:80)
       /api/user/  →  upstream user-svc   ←── q_upstream_sync 维护
       /api/order/ →  upstream order-svc  ←── q_upstream_sync 维护
              │
      ┌───────┴───────┐
   user-svc:8081   user-svc:8082     （多实例，Consul 里自动发现）
```

`q_upstream_sync` 直接复用 `libq_reg`，每 3 秒拉一次健康实例，
**内容变了才写盘并 reload nginx**（没变化不动，避免无意义的 reload）。

```bash
q_upstream_sync 127.0.0.1:8500 /usr/local/etc/nginx/upstream \
    --reload-cmd "nginx -s reload" user-svc order-svc
```

生成 `upstream/user-svc.conf`：

```nginx
upstream user-svc {
    server 10.0.0.5:8081 max_fails=3 fail_timeout=10s;
    server 10.0.0.6:8081 max_fails=3 fail_timeout=10s;
    keepalive 64;
}
```

实例全部下线时写一个 `server 127.0.0.1:1 down;` 占位 —— 否则 nginx 的
upstream 块为空会直接配置校验失败。

nginx 侧配 `proxy_http_version 1.1` + `proxy_set_header Connection ""`
配合 upstream 的 `keepalive 64`，接入层到后端才走长连接。

---

## 5. 冒烟结果（samples/user_svc，mock 驱动）

```
GET    /user/1        -> {"code":0,"data":null}
GET    /user/search   -> {"code":0,"data":[]}
POST   /user          -> {"code":0,"data":{"id":1}}
POST   /user          -> {"code":0,"data":{"id":2}}      # insert_id 自增
PUT    /user/5        -> {"code":0,"data":{"affected":1}}
DELETE /user/5        -> {"code":0,"data":{"affected":1}}
POST   /user (坏 JSON) -> {"code":400,"msg":"body 不是合法 JSON"}
GET    /nope          -> {"error":"not found"}
SIGTERM               -> shutting down（优雅退出）
```

---

## 6. 本轮修掉的 bug

1. **libcurl 继承了环境里的代理** —— 这台机器有 `http_proxy=http://127.0.0.1:49939`，
   libcurl 默认读取，导致内网调用被代理截走，请求行变成绝对 URI，对端直接 404。
   现象很迷惑：**第一次请求成功，之后全 404**（`curl_easy_reset` 会把代理恢复成
   「读环境变量」）。已在 `q_httpc_new()` 和每次 `q_httpc_do()` 的 reset 之后
   显式 `CURLOPT_PROXY=""` + `CURLOPT_NOPROXY="*"`。
   内网调用走代理本身就是错的，这条应该是默认值。
2. **路由顺序** —— `/user/:id` 会吃掉 `/user/search`。已调整注册顺序，
   并在代码里写了注释（后面可以改成按精确度排序，不依赖调用顺序）。
3. **MySQL 驱动可选** —— `user_svc` 硬引用 `q_db_register_mysql`，
   没装 connector-c 的机器链接失败。改成 CMake 检测 `TARGET q_db_mysql`
   再定义 `Q_HAVE_MYSQL`。

---

## 7. 已知限制 / 下一步

- `q_reg` 目前只在 refresh 线程里刷新自己 watch 过的服务，没有用 Consul 的
  blocking query（`?index=`）做长轮询，实时性取决于 `refresh_secs`。
- 负载均衡只有轮询，没有权重 / 最少连接 / 一致性哈希。
- 没有熔断器：实例连续失败不会临时摘除（nginx 侧有 `max_fails` 兜底）。
- nginx 配置是样例，没在真 nginx 上验证过（这台机器没装 nginx）。
- 还没写过 Oracle / 达梦驱动 —— `q_db_ops_t` 接口已经按这个目标设计，
  加驱动不需要动 mapper 和业务代码。

下一步建议：
1. 找台有 MySQL 的机器跑一遍 `user_svc` 的真实数据库冒烟
2. blocking query 替换定时刷新
3. Oracle 驱动（`q_db_ops_t` 的第二个实现）
