#ifndef Q_CONF_H
#define Q_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ini 配置读取。
 * 取值顺序：环境变量 > 配置文件 > 默认值。
 * 环境变量名规则：Q_<SECTION>_<KEY>（全大写，如 Q_SERVER_PORT）。
 */

typedef struct q_conf q_conf_t;

q_conf_t  *q_conf_load(const char *path);
q_conf_t  *q_conf_new(void);
void       q_conf_free(q_conf_t *c);

int         q_conf_set(q_conf_t *c, const char *sec, const char *key, const char *val);

const char *q_conf_get(const q_conf_t *c, const char *sec, const char *key, const char *def);
int         q_conf_get_int(const q_conf_t *c, const char *sec, const char *key, int def);
long long   q_conf_get_ll(const q_conf_t *c, const char *sec, const char *key, long long def);
int         q_conf_get_bool(const q_conf_t *c, const char *sec, const char *key, int def);
double      q_conf_get_double(const q_conf_t *c, const char *sec, const char *key, double def);

int         q_conf_has(const q_conf_t *c, const char *sec, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* Q_CONF_H */
