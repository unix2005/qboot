# qboot M0 进度报告

> 状态：M0 主体完成（core / log / conf / http），已跑通 curl 与并发压测。
> 环境：macOS + clang 17 + cmake 4.4（开发调试用）；目标平台 Linux（含麒麟/统信）。

---

## 1. 已完成

| 模块 | 内容 | 状态 |
|---|---|---|
| 构建骨架 | 顶层 CMake、`-Wall -Wextra -Wshadow`、ASan/TSan/UBSan 开关、依赖缺失自动跳模块 | 完成 |
| `libq_core` | 内存池（bump + 大块分离 + reset 复用）、strbuf、动态数组、哈希表、时间、线程池、错误码 | 完成，44 项测试通过 |
| `libq_log` | 级别/分类/轮转/trace 上下文、无锁环形队列、后台线程批量落盘、降级与崩溃安全 | 完成，100 万条压测 783ms |
| `libq_conf` | ini 解析、环境变量覆盖（`Q_<SEC>_<KEY>`）、类型化取值 | 完成，12 项测试通过 |
| `libq_http` | libevent 多路复用 + llhttp 解析、每 IO 线程一个 event_base + SO_REUSEPORT、路由（含 `:param`）、`/health` | 完成，20000 请求压测 47k QPS |
| 示例 | `demo_svc`（线程池 + 日志 + 配置 + 优雅退出）、`http_demo`（HTTP 服务） | 完成 |

目录：

```
qboot/
├── CMakeLists.txt
├── core/    → libq_core   （mem / str / ds / time / threadpool / types）
├── log/     → libq_log    （异步日志）
├── conf/    → libq_conf   （ini + 环境变量）
├── http/    → libq_http   （libevent + llhttp）
├── deps/llhttp/           （v9.2.1，随仓库自带）
├── samples/               （demo_svc / http_demo / conf/app.ini）
└── tests/                 （test_core / test_log / test_conf）
```

---

## 2. 构建与运行

```bash
cd qboot
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DQ_BUILD_HTTP=ON
cmake --build build -j4

./build/tests/test_core                 # core 单元测试
./build/tests/test_conf                 # 配置单元测试
./build/tests/test_log                  # 日志并发 + 行完整性校验
./build/tests/test_log bench            # 100 万条压测

./build/samples/demo_svc samples/conf/app.ini     # 线程池 + 日志示例
./build/samples/http_demo samples/conf/app.ini    # HTTP 服务，:8081
```

HTTP 验证：

```bash
curl -i http://127.0.0.1:8081/health              # 框架内置
curl    http://127.0.0.1:8081/api/users/42        # 路径参数
curl    "http://127.0.0.1:8081/api/search?q=x"    # query 参数
curl -X POST -d '{"a":1}' http://127.0.0.1:8081/api/echo
Q_LOG_LEVEL=debug ./build/samples/http_demo samples/conf/app.ini   # 环境变量覆盖配置
```

---

## 3. 测试数据

| 项 | 结果 |
|---|---|
| `test_core` | 44 passed / 0 failed |
| `test_conf` | 12 passed / 0 failed |
| 日志并发（8 线程 × 2000） | 16001 行，**行撕裂 0，丢弃 0** |
| 日志压测（4 线程 × 25 万） | 100 万条 **783ms（1.28M msg/s）**，丢弃 0，同步降级 0 |
| HTTP 压测（8 连接 keep-alive） | 20000 请求 **0.423s，约 47k QPS**，错误 0 |
| 日志输出 | 分类文件（app / access / sql）+ trace_id + tid，格式统一 |

> 以上均为 `-O0` Debug 构建下的数字，Release 会更高。

---

## 4. 过程中修掉的两个真问题

**1）日志队列满导致大量丢弃（压测暴露）**
原实现每条日志一次 `write(2)`，消费者跟不上生产者，16000 条丢了 10052 条。
修法两步：① 每个分类一个 64KB 写缓冲，一批合并成一次 `write`；② 队列满时给消费者 2ms 等待窗口形成背压，超时才降级。修完：**丢弃 0**。

**2）keep-alive 第二个请求 400（压测暴露）**
在 llhttp 的回调栈内调用了 `llhttp_reset()`，破坏解析状态机。
修法：回调里只打标记，`llhttp_execute()` 返回后再 reset。修完：单连接连续请求全部 200。

---

## 5. 已知限制（下一步处理）

1. **不支持 HTTP pipelining**——同一读缓冲内的多个请求只会处理第一个；串行 keep-alive 正常。要支持需按 `llhttp_get_error_pos()` 计算偏移量循环 execute。
2. **JSON 还没接入 jansson**——示例里是手工拼字符串，POST 回显没做转义。M2 接 jansson 后统一。
3. **HTTP 层没有访问日志中间件**——access 分类目前只在示例里手写，应在 `libq_http` 里做成中间件自动打。
4. **无请求超时、无 body 大小限制、无连接数上限**——生产前必须补，否则慢客户端能拖死服务。
5. **日志轮转只按大小**，`keep_days` 已存但过期清理线程未实现。
6. **没有单元测试覆盖 `libq_http`**——只有手工 curl 和压测脚本，应补进 CTest。

---

## 6. 下一步（按里程碑）

- **M1 数据库**：`libq_db` 抽象 + MariaDB Connector/C 驱动 + 预编译 + 连接池（ThreadLocal + 分段池）+ 事务 + 慢 SQL
- **M2 Mapper**：XML 解析 AST、动态标签引擎、`#{}`/`${}`、结果 → JSON/struct、`gen_meta.py`
- **M3 服务治理**：Consul 注册/发现、nginx + consul-template、服务间调用 + 负载均衡
- 顺带补掉上面第 1~6 条限制

开始前需要你确认的两件小事：
- `libq_db` 是否先用 macOS 上的 MySQL/MariaDB 客户端库验证（需要 `brew install mariadb-connector-c`），还是直接按接口写完等上 Linux 再测。
- 动态标签范围是否就按 `if/choose/where/set/foreach/include` 这一档。
