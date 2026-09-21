# 用户级长期记忆

## 中国石油算法挑战赛（多道题并行参与）

用户同时参加该赛事的多道赛题，工作区按题分目录。以下为跨题通用规则：

- **提交结果 CSV 一律用 UTF-8 无 BOM**（`encoding="utf-8"`），**不要**用 `utf-8-sig`。
  pandas 会自动吞 BOM，但平台校验器用 `csv` 模块严格比对时会把首列读成 `﻿WELL_ID`，
  判定「缺少 WELL_ID」并报 `exit=3`（submissionDir 中没有合格 CSV）。
- **自查必须用 `csv` 模块，不能用 pandas** —— pandas 太宽容会掩盖 BOM 问题。
  可复用工具：任一工作区下的 `check_submission.py`（支持 zip / 目录两种 target）。
- **除 zip 外，把结果 CSV 同时散落到输出目录**：部分流水线是「扫描 submissionDir 找 CSV」而非收 zip。
- 提交脚本必须支持**零参数运行**（自动探测数据目录 + 默认输出路径），
  不能把 `--data_dir`/`--output` 设成 `required=True`，否则平台直接 `python predict.py` 会报错。
- 提交代码包只需 `README.md` + `predict.py` + `requirements.txt`；
  `config.yaml`、`src/`、`models/`、`examples/` 均为可选，不带 pkl 可避免 sklearn 版本绑定失败。

## 个人编码习惯（跨项目通用）

- 资深 C 程序员，自写程序**命名一律以 `q_` 开头**（函数、类型），**宏与枚举用 `Q_` 前缀**。
  新建库/模块时沿用此约定（如 `libq_core`、`q_conn_t`、`Q_ROUTE_GET`），不要发明别的前缀。
- 他给模型起的名字是「浑元」，用于标记产出由 AI 完成；因此 `hy` 前缀属于模型侧标记，**工程代码里不要用 hy 开头**。
- 老派 C 工程组织习惯：`api/` + `headers.h` + `makefile`。新项目可以建议改成 `include/` + `src/` + CMake，但要说明理由。
- 技术取向：能自研就自研，第三方依赖越少越好；引入库先看开源协议（闭源交付场景，GPL/LGPL 要区分清楚）。
- 沟通偏好：先给判断再说理由，不要反复追问；关键取舍要列出代价让他拍板。
