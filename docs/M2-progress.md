# M2 进展：XML Mapper（外置 SQL）

状态：**完成并通过端到端自测（49/49）**

---

## 1. 交付内容

| 模块 | 路径 | 说明 |
|---|---|---|
| `libq_mapper` | `mapper/` | XML → AST，运行期动态标签求值 + 参数绑定 + 结果映射 |
| `libq_db_mock` | `db/driver/mock/` | 内存假驱动，无真实数据库也能跑全链路 |
| 样例 mapper | `tests/mapper/User.xml` | 覆盖全部动态标签 |
| 端到端测试 | `tests/test_mapper.c` | 49 项断言 |

编译：

```bash
cmake -S . -B build -DQ_BUILD_HTTP=ON
cmake --build build -j8
./build/tests/test_mapper tests/mapper/User.xml
```

---

## 2. 支持的 XML 语法

| 标签/语法 | 支持 | 备注 |
|---|---|---|
| `<select>/<insert>/<update>/<delete>` | ✅ | `namespace` 属性生成 `ns.id` 形式的语句 id |
| `<sql id>` + `<include refid>` | ✅ | 启动期展开，运行期零开销 |
| `<if test>` | ✅ | |
| `<choose>/<when>/<otherwise>` | ✅ | 只命中第一个成立的 when |
| `<where>` | ✅ | 自动去掉首个 AND/OR；条件全空则整段消失 |
| `<set>` | ✅ | 自动去掉尾逗号 |
| `<foreach collection item open close separator>` | ✅ | 每一项单独渲染后 trim |
| `#{name}` | ✅ | 转成 `?` 占位符 + 绑定参数 |
| `${name}` | ⚠️ | 直接拼接，打 WARNING，仅用于白名单标识符 |

`<if test>` 表达式支持：路径（`a.b.c`）、数字、字符串字面量、`null/true/false`、
`== != > >= < <=`、`and/or/not`、括号。

---

## 3. 生成的 SQL 实例

输入 XML：

```xml
<select id="search">
    SELECT <include refid="Base_Column_List"/>
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
    ORDER BY ${orderBy}
    LIMIT #{offset}, #{size}
</select>
```

调用 `q_ctx_query(c, "user.search", params, &rows, ...)`，实际下发：

```sql
SELECT id, user_name, age, balance, status, created_at FROM t_user
WHERE user_name LIKE ? AND age >= ? AND status IN (?,?,?)
ORDER BY id DESC LIMIT ?, ?
```

绑定参数 7 个。条件全部为空时：

```sql
SELECT id, user_name, age, balance, status, created_at FROM t_user ORDER BY id LIMIT ?, ?
```

---

## 4. 运行期设计要点

- **启动期解析、运行期零解析**：XML 只在 `q_mapper_load()` 时读一次，变成内存 AST。
- **预处理语句按连接缓存**：`q_conn_t.ud` 挂一个小哈希表（SQL 文本 → `MYSQL_STMT`）。
  自测里连续 5 次同一查询，`prepare` 只调 1 次。
- **事务**：`q_ctx_tx()` 拿 ThreadLocal 连接，首次自动 `BEGIN`，commit/rollback 后自动归还。
- **慢 SQL**：超过 200ms 自动打 `sql` 分类的 WARN 日志，带上生成的完整 SQL。
- **结果映射**：JSON 数组（`q_ctx_query`）和 struct 数组（`q_ctx_query_struct`）两条路。
  struct 用 `Q_FIELD(type, member, ftype)` 宏声明列与字段的对应关系。

---

## 5. 本轮修掉的 bug（都是真问题，不是测试问题）

1. **文本节点空白被吞** —— 用了 `XML_PARSE_NOBLANKS`，`<include>` 前后的缩进整段消失，
   拼出 `SELECTid, user_nameFROM t_user`。改成保留空白节点，最终由 `sql_tidy()` 收尾
   （引号内的空白原样保留，避免改坏 `'a  b'` 这种字面量）。
2. **`expr.c` 释放了不属于自己的内存** —— 字符串字面量 `malloc` 出来的缓冲和
   jansson 内部的字符串指针混在一起 `free`。给 `ev_t` 加了 `own_str` 标记。
3. **关键字没有词边界** —— `and` 会吃掉 `android`、`or` 会吃掉 `order`。加了边界判断。
4. **数组/对象被判成 null** —— `<if test="list != null">` 对数组恒不成立。
   `from_json()` 补上数组/对象：非空即真。
5. **`foreach` 每项带多余空白** —— 拼出 `( ? , ? , ? )`。改成每项单独渲染后 trim。
6. **`db.c` 的 `conn_destroy` 隐式声明** —— 定义在 `q_dbp_free` 之后，加前向声明。
7. **`db.h` 没有带 `q/core/types.h`** —— 公共头文件返回 `Q_OK/Q_ERR_*` 却不提供定义，
   调用方得自己猜。已补上。
8. **mock 的 `insert_id` 回填早于自增** —— 调用方永远拿到上一次的值。
9. **mock 保存的参数指针悬空** —— mapper 绑完就释放自己的缓冲，mock 里留的是野指针。
   改成拷进自己的静态池。

---

## 6. 已知限制 / 下一步

- `${}` 只做了告警，没有做标识符白名单校验（M3 之前补）。
- `<if test>` 不支持方法调用（MyBatis 的 `list.size() > 0` 之类），需要时再扩。
- 没有做语句 id 重复的检测，后加载的会覆盖先加载的。
- 真实 MySQL 还没跑过：这台机器装不上 MariaDB server（brew 被 sandbox 拦），
  只有 connector-c。M3 会补一个连线冒烟脚本，在有库的机器上一条命令验证。

下一步：**M3 —— Consul 服务注册与发现 + nginx 接入 + 服务间调用。**
