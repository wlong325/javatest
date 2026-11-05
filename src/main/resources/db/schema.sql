-- ============================================================
-- 嵌入式综合环境监测台 · 数据库建表脚本
--
-- 用途：在新机器上从零还原数据库结构，让仓库真正可复现。
-- 环境：MySQL 8.0，字符集 utf8mb4
--
-- 执行方式（任选其一）：
--   1) 命令行   mysql -u root -p < src/main/resources/db/schema.sql
--   2) Navicat  右键连接 → 运行 SQL 文件 → 选中本文件
--   3) IDEA     打开本文件 → 全选 → 点右上角绿色三角执行
--
-- 全部语句带 IF NOT EXISTS，重复执行不会报错，也不会清掉已有数据。
--
-- 为什么放在 db/ 子目录而不是 resources 根目录：
--   Spring Boot 启动时会去 classpath 根目录找 schema.sql 并自动执行，
--   放在 db/ 下面就不会被误触发，建表完全由你手动控制。
-- ============================================================


-- 第一步：建库
CREATE DATABASE IF NOT EXISTS `javatest`
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_general_ci;

USE `javatest`;


-- 第二步：建设备数据表
-- 板子每 2 秒上报一帧 "S,温度,光照,电位器,频率"，对应一行记录。
-- 前端曲线和"最新值"都从这张表读，TcpServer 收到数据后写入。
CREATE TABLE IF NOT EXISTS `sensor_data` (
    `id`          INT          NOT NULL AUTO_INCREMENT     COMMENT '自增主键；查"最新一条"和"最近 N 条"都靠它排序',
    `temperature` DECIMAL(5,2) DEFAULT NULL                COMMENT '温度，单位 ℃，来自 DS18B20',
    `light`       DECIMAL(5,3) DEFAULT NULL                COMMENT '光敏电阻分压，单位 V，范围 0~5',
    `pot`         DECIMAL(5,3) DEFAULT NULL                COMMENT '电位器分压，单位 V，范围 0~5',
    `frequency`   INT          DEFAULT NULL                COMMENT 'NE555 输出频率，单位 Hz',
    `create_time` DATETIME     DEFAULT CURRENT_TIMESTAMP   COMMENT '入库时间，由数据库自动填，板子不上报时间',
    PRIMARY KEY (`id`)
) ENGINE = InnoDB
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_general_ci
  COMMENT = '环境采样记录，正常运行时每 2 秒新增一行';


-- 第三步：自检
-- 执行完上面两句后，跑一下这两条确认结构和权限都正常：
--
--   SHOW TABLES FROM javatest;                     -- 应看到 sensor_data
--   SELECT COUNT(*) FROM javatest.sensor_data;     -- 新库应为 0


-- 可选优化（现在不需要，数据攒多了再加）
-- 如果以后要做"只保留最近 7 天"的清理任务，按时间删数据会走全表扫描，
-- 加这条索引能让删除和按时间范围查询都快很多：
--
--   CREATE INDEX idx_create_time ON sensor_data (create_time);
