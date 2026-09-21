---
summary: "User profile record"
read_when:
  - Bootstrapping a workspace manually
---

# USER.md - About Your Human

- **Name:** qiaoshui
- **What to call them:** qiaoshui
- **Pronouns:** 未提及
- **City:** 未提及
- **Notes:** 资深 C 程序员。命名习惯 `q_` 前缀（函数、类型）与 `Q_`（宏、枚举）。老派 C 工程组织方式：`api/` + `headers.h` + `makefile`。

## Context

**在做什么**：用 C 写一套类 Spring Boot 的微服务框架（代号 `qboot`）——nginx 接入、XML 外置 SQL（MyBatis 风格）、MySQL/Oracle/达梦多库、Consul 服务注册发现，按模块化拆成独立库，业务实例链接库即可开发。一期只做 MySQL。

**已定决策**：单进程多线程；MySQL 客户端用 MariaDB Connector/C（避开 GPL）；不做 Windows（Linux 含麒麟/统信 + macOS 调试）；日志自研，不用 zlog。

**偏好与判断方式**

- 能自研就自研，第三方依赖越少越好；选库先看协议（闭源交付场景，警惕 GPL）
- 有现成代码（如他自己的 `q_log`）时，倾向"保留接口、换掉实现"，而不是整套推翻
- 不希望被反复追问，要我先做判断；但关键取舍要摆出来让他拍板
- 说话直接，喜欢带具体行号、命令、可跑的代码，不接受空泛的架构词

---

他给模型起名"浑元"是为了记录活是谁干的。涉及历史：2026-09 起参与 qboot 框架从零设计。
