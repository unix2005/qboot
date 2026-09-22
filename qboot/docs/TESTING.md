# qboot 测试手册

分两部分：**A** 是这台 Mac 上已经跑过并全部通过的部分；**B** 是拷到 Ubuntu 之后
要跑的部分，每条都给了命令和预期输出，照着做即可。

## A. 本机（macOS）已验证

环境：macOS + clang，libevent / libcurl / libxml2 / jansson / mariadb-connector-c
（brew，keg-only）、**没有 MariaDB Server、没有 Consul、没有 pkg-config**。

### A.1 构建

```bash
cd qboot
cmake -S . -B build -DQ_BUILD_HTTP=ON
cmake --build build -j8
```

实际输出（尾部）：

```
[ 89%] Linking C shared library lib/libq_http.dylib
[ 91%] Linking C static library ../lib/libq_http.a
[100%] Built target test_reg
```

`build/lib/` 下 **10 个 `.a` + 10 组 `.dylib`**（`libq_x.dylib -> libq_x.0.dylib -> libq_x.0.3.0.dylib`）：

```
libq_conf  libq_core  libq_db  libq_db_mock  libq_db_mysql
libq_http  libq_httpc  libq_log  libq_mapper  libq_reg
```

### A.2 单元测试

```bash
bash scripts/test_all.sh
```

实际输出：

```
--- 库产物检查
[   OK  ] 静态库 .a: 10 个
[   OK  ] 动态库 .dylib(含软链): 30 个

--- 单元测试
[   OK  ] test_conf      pass=12
[   OK  ] test_core      pass=44
[   OK  ] test_log       pass=4
[   OK  ] test_mapper    pass=49
[   OK  ] test_reg       pass=28

--- 汇总
  测试程序: 5 个
  断言通过: 137
  断言失败: 0

--- 样例服务启动自检（3 秒）
[   OK  ] GET /health
[   OK  ] 全部通过
```

ASan 版本同样全过（14s）：

```bash
bash scripts/test_all.sh --asan
```

### A.3 安装 + 外部项目接入

```bash
cmake --install build --prefix /tmp/qboot-install
```

三种接入方式都验过：

**① find_package（静态）**

```bash
cd templates/minimal_svc
cmake -S . -B build -Dqboot_DIR=/tmp/qboot-install/lib/cmake/qboot
cmake --build build
otool -L build/minimal_svc | grep -c libq_      # => 0（真静态，没链 dylib）
```

**② find_package（动态）**

```bash
cmake -S . -B build -Dqboot_DIR=/tmp/qboot-install/lib/cmake/qboot -DQBOOT_USE_SHARED=ON
cmake --build build
otool -L build/minimal_svc | grep libq_ | head -3
# 	@rpath/libq_http.0.dylib ...
# 	@rpath/libq_conf.0.dylib ...
```

**③ 手工 cc 链接**

```bash
cc -o minimal_svc minimal_svc.c -I/tmp/qboot-install/include \
   -L/tmp/qboot-install/lib \
   -lq_http -lq_mapper -lq_reg -lq_httpc -lq_db_mysql -lq_db_mock -lq_db \
   -lq_conf -lq_log -lq_core \
   -levent -levent_pthreads -lcurl -lxml2 -ljansson -lmariadb -lpthread
```

跑起来：

```bash
./minimal_svc app.ini &
curl -s --noproxy '*' http://127.0.0.1:9090/health     # {"status":"up"}
curl -s --noproxy '*' http://127.0.0.1:9090/hello      # {"msg":"hello from minimal-svc"}
curl -s --noproxy '*' -XPOST -d '{"a":1}' http://127.0.0.1:9090/echo   # {"a":1}
```

### A.4 端到端 demo（mock 驱动，无需数据库）

```bash
bash scripts/run_demo.sh
```

实际输出（连跑三次，退出码都是 0）：

```
--- 启动服务
[   OK  ] user-svc :8081   order-svc :8082

--- 接口演示
  user-svc  查单个    {"code":0,"data":{"id":1,"user_name":"zhangsan","age":20,...}}
  user-svc  条件查询  {"code":0,"data":[{...zhangsan...},{...lisi...}]}
  user-svc  新增      {"code":0,"data":{"id":1}}
  order-svc 列表      {"code":0,"data":[{"id":1,"item":"键盘",...},{"id":2,"item":"显示器",...}]}
  order-svc 跨服务    {"code":0,"data":{"id":1,"user_name":"zhangsan",...}}
  order-svc 建单      {"code":0,"data":{"id":1}}
  健康检查            {"status":"up"}

--- 断言
[   OK  ] ...
[   OK  ] demo 跑完，进程已清理
```

### A.5 修掉的两个真 bug（ASan 立功）

1. **`json_ok()` 之后又 `json_decref()` → use-after-free**
   症状是服务偶发 `Abort trap: 6`。ASan 报：
   ```
   ERROR: AddressSanitizer: heap-use-after-free
       #0 json_decref jansson.h:132
       #1 h_search user_svc.c:126
   ```
   `json_object_set_new` 会接管引用所有权，之后不能再 decref。
   已在 `user_svc.c` / `order_svc.c` 修掉并加了注释。

2. **`wait_port` 里的 `exec 3<&- 2>/dev/null` 把脚本 stderr 永久重定向到 /dev/null**
   `exec` 的重定向是对当前 shell 永久生效的，导致脚本后面所有报错都"神秘消失"，
   只剩一个退出码 1。已改成在子 shell 里开 fd、不碰父 shell 的 stderr。

## B. Ubuntu 上要跑的（明天）

> 下面每条都假定当前目录是 `qboot/`。脚本自带 `--help`。

### B.1 装依赖

```bash
sudo ./scripts/install_deps_ubuntu.sh
```

预期输出：

```
--- 更新 apt 索引
--- 安装编译工具链与 pkg-config
--- 安装必选依赖库
--- 校验头文件与库是否到位
[   OK  ] build-essential
[   OK  ] cmake
[   OK  ] pkg-config
[   OK  ] libevent-dev
[   OK  ] libcurl4-openssl-dev
[   OK  ] libxml2-dev
[   OK  ] libjansson-dev
[   OK  ] libmariadb-dev
```

如果缺 consul / MySQL / nginx：

```bash
sudo ./scripts/install_deps_ubuntu.sh --with-consul --with-mysql-server --with-nginx
```

### B.2 编译安装

```bash
./scripts/build.sh
```

预期：

```
--- 依赖检查
[   OK  ] event2/event.h
[   OK  ] curl/curl.h
[   OK  ] libxml2/libxml/xmlversion.h
[   OK  ] jansson.h
--- 产物
--- 静态库 (.a) ---
libq_conf.a libq_core.a ... 共 10 个
--- 动态库 (.so/.dylib) ---
libq_conf.so libq_core.so ... 
--- 安装到 /usr/local
[   OK  ] 已安装
--- 验证安装结果
[   OK  ] qboot 0.3.0: -L/usr/local/lib -lq_http ... -lq_core
[   OK  ] find_package(qboot) 可用: -Dqboot_DIR=/usr/local/lib/cmake/qboot
```

### B.3 单元测试

```bash
./scripts/test_all.sh
```

**预期：137 项断言全过**，与 A.2 一致。有失败会打印 `[ FAIL ]` 并给出重跑命令。

ASan 再跑一遍（Ubuntu 上支持 leak 检测，比 macOS 更严格）：

```bash
./scripts/test_all.sh --asan
```

> leak 检测会发现"服务进程启动后没释放的池/上下文"这类东西；
> 单元测试里应该 0 报告。若有 leak，把 `ASAN_OPTIONS=detect_leaks=1` 的堆栈贴出来。

### B.4 mock 驱动 demo（不依赖数据库，先跑这个确认环境 OK）

```bash
./scripts/run_demo.sh --build
```

预期与 A.4 一致，退出码 0。

想留着进程自己 curl：

```bash
./scripts/run_demo.sh --keep
# 另开终端：
curl http://127.0.0.1:8081/user/1
curl -XPOST http://127.0.0.1:8082/order -d '{"userId":1,"item":"鼠标","amount":99.9}'
```

### B.5 真 MySQL/MariaDB 冒烟（重点，本机做不了）

```bash
sudo ./scripts/install_deps_ubuntu.sh --with-mysql-server
sudo mysql < sql/qboot_demo.sql                 # 建库 qboot_demo + t_user/t_order + 种子数据
```

`sql/qboot_demo.sql` 结尾会打印：

```
result
qboot_demo 初始化完成
users
3
orders
2
```

然后：

```bash
./scripts/smoke_mysql.sh --user root --pass ''
```

如果 root 走 unix socket 需要 sudo：

```bash
./scripts/smoke_mysql.sh --sudo
```

预期输出（关键行）：

```
--- 数据库连通性
[   OK  ] 连上了: 10.x.x-MariaDB
--- 建库建表 sql/qboot_demo.sql
[   OK  ] 表已就绪
[   OK  ] t_user 现有 3 行
--- 启动服务
[   OK  ] user-svc :8081 / order-svc :8082 已就绪
--- user-svc 增删改查
[   OK  ] GET /user/1（种子数据 zhangsan）
[   OK  ] GET /user/search?name=zhang%25（LIKE 动态标签）
[   OK  ] GET /user/search?minAge=26（应只剩 wangwu，30 岁）
[   OK  ] POST /user 成功，新 id=4
[   OK  ] GET /user/4（回查刚插入的行）
[   OK  ] PUT /user/4 后 age=19
[   OK  ] DELETE /user/4 生效
--- order-svc 跨服务调用
[   OK  ] order-svc -> user-svc 透传
[   OK  ] GET /order/list?userId=1
[   OK  ] POST /order 后列表里有鼠标
--- 核对落库结果
（打印 t_user / t_order 的实际内容）
--- 通过
```

自查 SQL（脚本之外，手动确认用）：

```bash
mysql -uroot qboot_demo -e "SELECT * FROM t_user; SELECT * FROM t_order;"
```

### B.6 Consul 冒烟（重点，本机做不了）

```bash
sudo ./scripts/install_deps_ubuntu.sh --with-consul
consul agent -dev -client=0.0.0.0 > /tmp/consul.log 2>&1 &
# 或者让脚本自己起：
./scripts/smoke_consul.sh --start
```

```bash
./scripts/smoke_consul.sh
```

预期输出（关键行）：

```
--- Consul 连通性
[   OK  ] Consul leader: "127.0.0.1:8300"
--- 启动两个服务并注册
[   OK  ] user-svc 在 Consul 里已是 passing
[   OK  ] order-svc 也已注册
--- 用服务名调用（不写死地址）
[   OK  ] order-svc --(user-svc)--> /user/1
[   OK  ] POST /order（内部会先问 user-svc）
--- 服务目录一览
    consul
    order-svc
    user-svc
--- 优雅退出 -> 自动摘除
[   OK  ] 实例已从健康列表移除
```

手动确认：

```bash
curl -s http://127.0.0.1:8500/v1/catalog/services
curl -s 'http://127.0.0.1:8500/v1/health/service/user-svc?passing' | head -40
```

### B.7 nginx 接入（可选）

```bash
sudo ./scripts/install_deps_ubuntu.sh --with-nginx
sudo mkdir -p /etc/nginx/upstream
./build/bin/q_upstream_sync 127.0.0.1:8500 /etc/nginx/upstream \
    --reload-cmd "nginx -s reload" user-svc &
sudo nginx -c $(pwd)/deploy/nginx/nginx.conf
curl -s http://127.0.0.1:8080/user/1
```

## C. 脚本一览

| 脚本 | 用途 | 关键选项 |
|---|---|---|
| `install_deps_ubuntu.sh` | 装依赖（只装包，不编译） | `--with-consul --with-mysql-server --with-nginx --dry-run` |
| `build.sh` | 编译 + 安装 | `--prefix --type --jobs --asan --shared 0 --clean --no-install` |
| `test_all.sh` | 跑全部单元测试 + 产物检查 | `--build --asan --filter mapper --verbose` |
| `run_demo.sh` | 一键起 demo 跑一遍 | `--keep --build --mode mysql` |
| `smoke_mysql.sh` | 真库端到端 | `--user --pass --host --port --db --sudo --skip-ddl` |
| `smoke_consul.sh` | Consul 端到端 | `--start --consul host:port` |
| `common.sh` | 公共库（被 source，不单独跑） | — |

所有脚本：出错立即停，并打印**可直接复制的修复命令**；后台起的进程在脚本退出时
自动回收（`trap cleanup_pids EXIT`）。调试时设
`QBOOT_BG_LOG_DIR=/tmp/qblog` 可以把后台服务 stdout/stderr 存成文件。

## D. 故障排查

| 现象 | 原因 | 修复 |
|---|---|---|
| `libevent not found -> skip libq_http` | 没装 libevent | `sudo apt install libevent-dev` 后 `./scripts/build.sh --clean` |
| `ld: cannot find -lmariadb` | connector-c 不在标准路径 | 看 `qboot.pc` 的 `Libs.private` 是否带 `-L`；没有就 `apt install libmariadb-dev` |
| 测试报 `找不到 build/bin` | 没编译 | `./scripts/build.sh` |
| `smoke_mysql.sh` 连不上 | mariadb 没起 / root 要 sudo | `sudo systemctl start mariadb` 或 `--sudo` |
| `smoke_mysql.sh` 里 DELETE 后回查还有数据 | 用的是 mock 驱动 | 确认 ini 里 db url 是 `mysql://`，不是 `mock://` |
| Consul 注册了但一直不 passing | TTL 没到 / 心跳线程没起来 | 等一个 ttl 周期；看服务日志 |
| `curl` 返回 404 或空 | 环境里有 `http_proxy` | 所有脚本里的 curl 都加了 `--noproxy '*'`；手写命令记得加 |
| 端口 8081/8082 被占 | 上次的服务没退干净 | 脚本退出会自动收；手动 `pkill -f user_svc` |
| demo 跑一半服务没了 | 早期版本的 use-after-free | 已修，确认用的是最新代码（含 `json_ok` 注释） |
