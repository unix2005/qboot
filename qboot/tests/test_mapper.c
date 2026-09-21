/*
 * mapper 端到端自测：XML 解析 -> 动态标签求值 -> 参数绑定 -> 执行 -> 结果映射。
 * 数据库用内存假驱动（libq_db_mock），因此不需要真实 MySQL 也能跑。
 */

#include <q/db.h>
#include <q/db_mock.h>
#include <q/log.h>
#include <q/mapper.h>

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
            printf("  [ok]   " fmt "\n", ##__VA_ARGS__);                   \
        } else {                                                           \
            g_fail++;                                                      \
            printf("  [FAIL] " fmt "\n", ##__VA_ARGS__);                   \
        }                                                                  \
    } while (0)

#define CHECK_EQ_STR(expect, actual)                                       \
    do {                                                                   \
        const char *_e = (expect);                                         \
        const char *_a = (actual);                                         \
        CHECK(_a != NULL && strcmp(_e, _a) == 0,                           \
              "expect [%s] got [%s]", _e, _a ? _a : "(null)");             \
    } while (0)

typedef struct {
    int      id;
    char     user_name[32];
    int      age;
    double   balance;
    int      status;
    char     created_at[32];
} user_t;

/* 归一化：去掉多余空白，方便断言 */
static char *squash(const char *s)
{
    static char buf[4][1024];
    static int  turn = 0;
    char *out = buf[turn++ & 3];

    size_t j = 0;
    int    pending = 0;
    for (size_t i = 0; s[i] != '\0' && j < sizeof(buf[0]) - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (j > 0) pending = 1;
            continue;
        }
        if (pending) { out[j++] = ' '; pending = 0; }
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return out;
}

static json_t *J(const char *s)
{
    json_error_t e;
    json_t *j = json_loads(s, 0, &e);
    if (j == NULL) {
        printf("bad json: %s (%s)\n", s, e.text);
        exit(1);
    }
    return j;
}

int main(int argc, char **argv)
{
    const char *xml = (argc > 1) ? argv[1] : "tests/mapper/User.xml";
    char        err[256];

    q_log_init("logs", "test_mapper", Q_LOG_WARN);

    q_db_register_mock();

    q_dbp_t *pool = q_dbp_new("mock://test:pwd@127.0.0.1:0/testdb", 4, 60, 1);
    if (pool == NULL) {
        printf("cannot create mock pool\n");
        return 1;
    }

    q_mapper_t *m = q_mapper_load(xml);
    if (m == NULL) {
        printf("cannot load mapper: %s\n", xml);
        return 1;
    }

    q_ctx_t *c = q_ctx_new(m, pool);

    printf("\n== 1. mapper 加载 ==\n");
    CHECK(q_mapper_size(m) == 7, "statements loaded = %d (expect 7)", q_mapper_size(m));
    CHECK(q_mapper_has(m, "user.selectById"), "has user.selectById");
    CHECK(q_mapper_has(m, "user.search"),     "has user.search");
    CHECK(!q_mapper_has(m, "user.nope"),      "unknown id not present");

    /* ---------------- 2. 最简单：#{} 参数绑定 ---------------- */
    printf("\n== 2. #{} 绑定 + <include> ==\n");
    {
        const char *cols[] = { "id", "user_name", "age", "balance", "status", "created_at" };
        qmock_cell_t cells[] = {
            QMOCK_INT(7), QMOCK_STR("zhangsan"), QMOCK_INT(30),
            QMOCK_DOUBLE(1234.5), QMOCK_INT(1), QMOCK_STR("2026-09-21 10:00:00")
        };
        qmock_expect(cols, 6, cells, 1);

        json_t *p = J("{\"id\":7}");
        json_t *rows = NULL;
        int rc = q_ctx_query(c, "user.selectById", p, &rows, err, sizeof(err));
        CHECK(rc == Q_OK, "query rc=%d%s%s", rc, rc ? " err=" : "", rc ? err : "");

        const char *sql = qmock_sql();
        printf("     sql: %s\n", sql);
        CHECK_EQ_STR("SELECT id, user_name, age, balance, status, created_at FROM t_user WHERE id = ?",
                     squash(sql));
        int np = 0;
        const q_value_t *ps = qmock_params(&np);
        CHECK(np == 1, "params=%d (expect 1)", np);
        CHECK(np == 1 && ps[0].type == Q_VAL_INT && ps[0].i64 == 7, "param0 = 7");

        CHECK(json_is_array(rows) && json_array_size(rows) == 1, "rows=1");
        json_t *r0 = json_array_get(rows, 0);
        CHECK(strcmp(json_string_value(json_object_get(r0, "user_name")), "zhangsan") == 0,
              "row.user_name = zhangsan");
        CHECK(json_integer_value(json_object_get(r0, "id")) == 7, "row.id = 7");
        CHECK(json_real_value(json_object_get(r0, "balance")) == 1234.5, "row.balance = 1234.5");

        json_decref(rows);
        json_decref(p);
    }

    /* ---------------- 3. <where> + <if> + <foreach> + ${} ---------------- */
    printf("\n== 3. <where>/<if>/<foreach>/${} ==\n");
    {
        qmock_reset();
        const char *cols[] = { "id" };
        qmock_cell_t cells[] = { QMOCK_INT(1) };
        qmock_expect(cols, 1, cells, 1);

        json_t *p = J("{\"name\":\"zhang%\",\"minAge\":18,"
                      "\"statusList\":[1,2,3],\"orderBy\":\"id DESC\","
                      "\"offset\":0,\"size\":20}");
        json_t *rows = NULL;
        int rc = q_ctx_query(c, "user.search", p, &rows, err, sizeof(err));
        CHECK(rc == Q_OK, "query rc=%d%s%s", rc, rc ? " err=" : "", rc ? err : "");

        const char *sql = squash(qmock_sql());
        printf("     sql: %s\n", sql);
        CHECK(strstr(sql, "WHERE user_name LIKE ?") != NULL, "<where> 去掉了首个 AND");
        CHECK(strstr(sql, "AND age >= ?") != NULL, "age 条件保留 AND");
        CHECK(strstr(sql, "AND status IN (?,?,?)") != NULL, "<foreach> 展开成 3 个占位符");
        CHECK(strstr(sql, "ORDER BY id DESC") != NULL, "${orderBy} 直接拼接");

        int np = 0;
        qmock_params(&np);
        CHECK(np == 7, "params=%d (expect 1+1+3+2=7)", np);

        json_decref(rows);
        json_decref(p);
    }

    /* ---------------- 4. 条件全不成立：<where> 整体消失 ---------------- */
    printf("\n== 4. 条件为空时 <where> 消失 ==\n");
    {
        qmock_reset();
        const char *cols[] = { "id" };
        qmock_cell_t cells[] = { QMOCK_INT(1) };
        qmock_expect(cols, 1, cells, 1);

        json_t *p = J("{\"orderBy\":\"id\",\"offset\":0,\"size\":10}");
        json_t *rows = NULL;
        int rc = q_ctx_query(c, "user.search", p, &rows, err, sizeof(err));
        CHECK(rc == Q_OK, "query rc=%d%s%s", rc, rc ? " err=" : "", rc ? err : "");

        const char *sql = squash(qmock_sql());
        printf("     sql: %s\n", sql);
        CHECK(strstr(sql, "WHERE") == NULL, "无条件时不产生 WHERE");
        int np = 0;
        qmock_params(&np);
        CHECK(np == 2, "params=%d (expect offset,size)", np);

        json_decref(rows);
        json_decref(p);
    }

    /* ---------------- 5. choose / when / otherwise ---------------- */
    printf("\n== 5. choose/when/otherwise ==\n");
    {
        qmock_reset();
        const char *cols[] = { "id" };
        qmock_cell_t cells[] = { QMOCK_INT(1) };
        qmock_expect(cols, 1, cells, 1);

        json_t *rows = NULL;
        json_t *p = J("{\"id\":5,\"name\":\"x\"}");
        q_ctx_query(c, "user.chooseOne", p, &rows, err, sizeof(err));
        CHECK(strstr(squash(qmock_sql()), "WHERE id = ?") != NULL, "命中第一个 when");
        json_decref(rows); json_decref(p);

        qmock_reset();
        qmock_expect(cols, 1, cells, 1);
        rows = NULL;
        p = J("{\"name\":\"x\"}");
        q_ctx_query(c, "user.chooseOne", p, &rows, err, sizeof(err));
        CHECK(strstr(squash(qmock_sql()), "WHERE user_name = ?") != NULL, "命中第二个 when");
        json_decref(rows); json_decref(p);

        qmock_reset();
        qmock_expect(cols, 1, cells, 1);
        rows = NULL;
        p = J("{}");
        q_ctx_query(c, "user.chooseOne", p, &rows, err, sizeof(err));
        CHECK(strstr(squash(qmock_sql()), "WHERE status = 1") != NULL, "落到 otherwise");
        json_decref(rows); json_decref(p);
    }

    /* ---------------- 6. insert ---------------- */
    printf("\n== 6. insert + insert_id ==\n");
    {
        qmock_reset();
        uint64_t iid = 0;
        json_t *p = J("{\"name\":\"lisi\",\"age\":22,\"balance\":88.5,\"status\":1}");
        int rc = q_ctx_insert(c, "user.insert", p, &iid, err, sizeof(err));
        CHECK(rc == Q_OK, "insert rc=%d%s%s", rc, rc ? " err=" : "", rc ? err : "");
        printf("     sql: %s\n", squash(qmock_sql()));
        int np = 0;
        const q_value_t *ps = qmock_params(&np);
        CHECK(np == 4, "params=%d (expect 4)", np);
        CHECK(np == 4 && strcmp(ps[0].str, "lisi") == 0, "param0 = lisi");
        CHECK(iid == 1, "insert_id=%llu (expect 1)", (unsigned long long)iid);
        json_decref(p);
    }

    /* ---------------- 7. <set> 去掉尾逗号 ---------------- */
    printf("\n== 7. <set> 去掉尾逗号 ==\n");
    {
        qmock_reset();
        uint64_t aff = 0;
        json_t *p = J("{\"id\":9,\"name\":\"wangwu\",\"age\":33}");
        int rc = q_ctx_exec(c, "user.updateSelective", p, &aff, NULL, err, sizeof(err));
        CHECK(rc == Q_OK, "update rc=%d%s%s", rc, rc ? " err=" : "", rc ? err : "");

        const char *sql = squash(qmock_sql());
        printf("     sql: %s\n", sql);
        CHECK(strstr(sql, "SET user_name = ?, age = ? WHERE id = ?") != NULL,
              "<set> 生成正确，尾逗号已去掉");
        int np = 0;
        qmock_params(&np);
        CHECK(np == 3, "params=%d (expect 3)", np);
        CHECK(aff == 1, "affected=%llu", (unsigned long long)aff);
        json_decref(p);
    }

    /* ---------------- 8. foreach 批量删除 ---------------- */
    printf("\n== 8. foreach 批量删除 ==\n");
    {
        qmock_reset();
        qmock_affected(3);
        uint64_t aff = 0;
        json_t *p = J("{\"ids\":[11,22,33]}");
        int rc = q_ctx_exec(c, "user.batchDelete", p, &aff, NULL, err, sizeof(err));
        CHECK(rc == Q_OK, "delete rc=%d%s%s", rc, rc ? " err=" : "", rc ? err : "");
        printf("     sql: %s\n", squash(qmock_sql()));
        CHECK(strstr(squash(qmock_sql()), "IN (?,?,?)") != NULL, "IN 展开 3 项");
        CHECK(aff == 3, "affected=%llu (expect 3)", (unsigned long long)aff);
        json_decref(p);
    }

    /* ---------------- 9. 语句缓存：同一 SQL 只 prepare 一次 ---------------- */
    printf("\n== 9. 语句缓存 ==\n");
    {
        qmock_reset();
        const char *cols[] = { "id" };
        qmock_cell_t cells[] = { QMOCK_INT(1) };
        qmock_expect(cols, 1, cells, 1);

        /* 换一个全新的池，连接是新的，语句缓存也就从零开始 */
        q_dbp_t *p2 = q_dbp_new("mock://test:pwd@127.0.0.1:0/testdb", 4, 60, 1);
        q_ctx_t *c2 = q_ctx_new(m, p2);

        for (int i = 0; i < 5; i++) {
            json_t *rows = NULL;
            json_t *p = J("{\"id\":1}");
            q_ctx_query(c2, "user.selectById", p, &rows, err, sizeof(err));
            json_decref(rows);
            json_decref(p);
        }
        CHECK(qmock_prepare_count() == 1, "prepare count = %d (expect 1, 走缓存)",
              qmock_prepare_count());
        CHECK(qmock_exec_count() == 5, "exec count = %d (expect 5)", qmock_exec_count());

        q_ctx_free(c2);
        q_dbp_free(p2);
    }

    /* ---------------- 10. 结果集 -> struct ---------------- */
    printf("\n== 10. 结果集映射到 struct ==\n");
    {
        qmock_reset();
        const char *cols[] = { "id", "user_name", "age", "balance", "status", "created_at" };
        qmock_cell_t cells[] = {
            QMOCK_INT(1), QMOCK_STR("aaa"), QMOCK_INT(20), QMOCK_DOUBLE(1.5),
            QMOCK_INT(1), QMOCK_STR("2026-01-01"),
            QMOCK_INT(2), QMOCK_STR("bbb"), QMOCK_INT(21), QMOCK_DOUBLE(2.5),
            QMOCK_INT(0), QMOCK_STR("2026-01-02")
        };
        qmock_expect(cols, 6, cells, 2);

        user_t list[8];
        memset(list, 0, sizeof(list));
        static const q_field_t fields[] = {
            Q_FIELD(user_t, id,         Q_F_INT),
            Q_FIELD(user_t, user_name,  Q_F_STR),
            Q_FIELD(user_t, age,        Q_F_INT),
            Q_FIELD(user_t, balance,    Q_F_DOUBLE),
            Q_FIELD(user_t, status,     Q_F_INT),
            Q_FIELD(user_t, created_at, Q_F_STR),
            Q_FIELD_END
        };

        json_t *p = J("{\"name\":\"a%\",\"orderBy\":\"id\",\"offset\":0,\"size\":10}");
        int n = q_ctx_query_struct(c, "user.search", p, fields, list,
                                   sizeof(user_t), 8, err, sizeof(err));
        CHECK(n == 2, "mapped rows=%d (expect 2)", n);
        CHECK(list[0].id == 1 && strcmp(list[0].user_name, "aaa") == 0,
              "row0: id=%d name=%s", list[0].id, list[0].user_name);
        CHECK(list[0].age == 20 && list[0].balance == 1.5, "row0: age=%d balance=%.1f",
              list[0].age, list[0].balance);
        CHECK(list[1].id == 2 && strcmp(list[1].user_name, "bbb") == 0 &&
              strcmp(list[1].created_at, "2026-01-02") == 0,
              "row1: id=%d name=%s created=%s",
              list[1].id, list[1].user_name, list[1].created_at);
        json_decref(p);
    }

    /* ---------------- 11. 错误处理 ---------------- */
    printf("\n== 11. 错误处理 ==\n");
    {
        json_t *rows = NULL;
        json_t *p = J("{}");
        int rc = q_ctx_query(c, "user.notExist", p, &rows, err, sizeof(err));
        CHECK(rc == Q_ERR_NOTFOUND, "unknown id -> Q_ERR_NOTFOUND (rc=%d)", rc);
        json_decref(p);

        qmock_reset();
        qmock_fail_next(1);
        p = J("{\"id\":1}");
        rc = q_ctx_query(c, "user.selectById", p, &rows, err, sizeof(err));
        CHECK(rc != Q_OK, "driver failure propagated (rc=%d, err=%s)", rc, err);
        json_decref(p);
    }

    /* ---------------- 12. 事务 ---------------- */
    printf("\n== 12. 事务 ==\n");
    {
        qmock_reset();
        q_conn_t *tx = q_ctx_tx(c, err, sizeof(err));
        CHECK(tx != NULL, "acquire tx conn");
        CHECK(qmock_tx("BEGIN") == 1, "BEGIN issued (%d)", qmock_tx("BEGIN"));

        uint64_t iid = 0;
        json_t *p = J("{\"name\":\"tx1\",\"age\":1,\"balance\":1,\"status\":1}");
        q_ctx_insert(c, "user.insert", p, &iid, err, sizeof(err));
        json_decref(p);

        CHECK(q_tx_commit(tx) == Q_OK, "commit ok");
        CHECK(qmock_tx("COMMIT") == 1, "COMMIT issued (%d)", qmock_tx("COMMIT"));

        qmock_reset();
        q_conn_t *tx2 = q_ctx_tx(c, err, sizeof(err));
        CHECK(q_tx_rollback(tx2) == Q_OK, "rollback ok");
        CHECK(qmock_tx("ROLLBACK") == 1, "ROLLBACK issued (%d)", qmock_tx("ROLLBACK"));
    }

    q_ctx_free(c);
    q_mapper_free(m);
    q_dbp_free(pool);
    q_log_close();

    printf("\n----------------------------------------\n");
    printf("mapper test: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
