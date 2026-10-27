package com.example.javatest.config;

import org.springframework.boot.CommandLineRunner;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Component;

/**
 * 项目每次启动时清空 sensor_data 表（演示/调试用，避免历史数据干扰）。
 * 如果以后要保留历史数据，删除本类或注释掉 run() 内的执行语句即可。
 */
@Component
public class DataCleanRunner implements CommandLineRunner {

    private final JdbcTemplate jdbcTemplate;

    public DataCleanRunner(JdbcTemplate jdbcTemplate) {
        this.jdbcTemplate = jdbcTemplate;
    }

    @Override
    public void run(String... args) {
        jdbcTemplate.execute("TRUNCATE TABLE sensor_data");
        System.out.println("[启动清理] sensor_data 表已清空（每次启动执行）");
    }
}
