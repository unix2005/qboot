# qboot 项目长期记忆

## 项目

`/Users/mac/work/q_c_hy_gateway/qboot/` —— 用 C 写的类 Spring Boot 微服务框架，代号 **qboot**。
nginx 接入 + XML 外置 SQL（MyBatis 风格）+ Consul 注册发现 + 模块化静态库。

## 命名约定（硬规则）

- 库：`libq_xxx`；函数/类型：`q_` 前缀；宏/枚举：`Q_` 前缀；头文件：`q/xxx.h`
- **绝对不要用 `hy` 前缀**。`hy` 是用户给模型起的名字「浑元」，只用于标记「这段是 AI 写的」，工程代码一律 `q_`。
  （用户自己写程序也习惯 `q_` 开头，因为名字叫 qiaoshui。）

## 已确认的技术决策

- 进程模型：**单进程多线程**（SO_REUSEPORT + 每 IO 线程一个 event_base + ThreadLocal DB 连接）
- MySQL 客户端：**MariaDB Connector/C**（LGPL，避开 libmysqlclient 的 GPL）
- 不做 Windows（Linux 含麒麟/统信 + macOS 调试）
- 日志：**自研 libq_log**，不用 zlog（环形队列 + 后台线程批量写）
- 注册中心：Consul（TTL check + 后台心跳）
- HTTP：入站 libevent + llhttp；出站 libcurl

## 模块一览

| 库 | 目录 | 状态 |
|---|---|---|
| libq_core | core/ | 完成，44 测试 |
| libq_log | log/ | 完成，100 万条 783ms 零丢弃 |
| libq_conf | conf/ | 完成，12 测试 |
| libq_http | http/ | 完成，~47k QPS |
| libq_db | db/ | 完成（ops vtable + dialect + 连接池 + 事务） |
| libq_db_mysql | db/driver/mysql/ | 编译通过，未连真库验证 |
| libq_db_mock | db/driver/mock/ | 完成，无库环境的自测/演示底座 |
| libq_mapper | mapper/ | 完成，49 测试 |
| libq_httpc | httpc/ | 完成 |
| libq_reg | registry/ | 完成，28 测试 |

进度文档：`docs/architecture-v1.md`、`M0-progress.md`、`M2-progress.md`、`M3-progress.md`

## 工程约定

- **没有数据库也要能自测**：本机装不上 MariaDB server（brew 被 sandbox-exec 拦），
  所有链路验证走 `libq_db_mock`；Consul 同理，测试里用 libq_http 起假 Consul。
- **可选依赖必须 CMake 兜底**：用 `if(TARGET xxx)` + `target_compile_definitions` 加宏，
  不能让没装依赖的机器链接失败（MySQL 驱动就是这么处理的）。
- 构建：`cmake -S . -B build -DQ_BUILD_HTTP=ON && cmake --build build -j8`
  （`option()` 有缓存，改默认值必须显式传 `-D`）
- ASan：`cmake -S . -B build-asan -DQ_ENABLE_ASAN=ON`（macOS 的 ASan 不支持 leak 检测，只查越界/UAF）

## 用户偏好

- 资深 C 程序员，要**可复现的、带命令和预期输出**的说明，不要泛泛而谈。
- 倾向**可复用、参数化**的脚本/模块，而不是一次性方案。
- 不确定时让我自己判断并做下去，不要反复追问。
- 说「你自己继续往下干」时是真的授权，按自己认为最合适的方案推进，最后给结果。
