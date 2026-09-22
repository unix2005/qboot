-- qboot demo 库表结构 + 种子数据
--
--   mysql -uroot -p < sql/qboot_demo.sql
--   或 MariaDB:  sudo mysql < sql/qboot_demo.sql
--
-- 表结构与 samples/mapper/User.xml、Order.xml 里的列名一一对应。
-- 改这里的列名就要同步改 XML，反过来也一样。

DROP DATABASE IF EXISTS qboot_demo;
CREATE DATABASE qboot_demo DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
USE qboot_demo;

-- ---------------- 用户表 ----------------
DROP TABLE IF EXISTS t_user;
CREATE TABLE t_user (
    id         BIGINT       NOT NULL AUTO_INCREMENT COMMENT '主键',
    user_name  VARCHAR(64)  NOT NULL                COMMENT '用户名',
    age        INT          NOT NULL DEFAULT 0      COMMENT '年龄',
    balance    DECIMAL(12,2) NOT NULL DEFAULT 0.00  COMMENT '余额',
    status     TINYINT      NOT NULL DEFAULT 1      COMMENT '1=正常 0=禁用',
    created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_user_name (user_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户';

INSERT INTO t_user (user_name, age, balance, status) VALUES
    ('zhangsan', 20, 100.50, 1),
    ('lisi',     25, 200.00, 1),
    ('wangwu',   30,   0.00, 0);

-- ---------------- 订单表 ----------------
DROP TABLE IF EXISTS t_order;
CREATE TABLE t_order (
    id         BIGINT        NOT NULL AUTO_INCREMENT COMMENT '主键',
    user_id    BIGINT        NOT NULL                COMMENT '下单用户',
    item       VARCHAR(128)  NOT NULL                COMMENT '商品名',
    amount     DECIMAL(12,2) NOT NULL DEFAULT 0.00   COMMENT '金额',
    status     TINYINT       NOT NULL DEFAULT 0      COMMENT '0=未支付 1=已支付',
    created_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='订单';

INSERT INTO t_order (user_id, item, amount, status) VALUES
    (1, '键盘',   299.50, 0),
    (1, '显示器', 1299.00, 1);

-- ---------------- 冒烟用账号（可选） ----------------
-- 不想用 root 跑冒烟就建一个：
--   CREATE USER 'qboot'@'%' IDENTIFIED BY 'qboot123';
--   GRANT ALL ON qboot_demo.* TO 'qboot'@'%';
--   FLUSH PRIVILEGES;
-- 然后：./scripts/smoke_mysql.sh --user qboot --pass qboot123

SELECT 'qboot_demo 初始化完成' AS result;
SELECT COUNT(*) AS users  FROM t_user;
SELECT COUNT(*) AS orders FROM t_order;
