package com.example.javatest.mapper;

import com.example.javatest.entity.SensorData;
import org.apache.ibatis.annotations.Mapper;

@Mapper
public interface SensorDataMapper {

    int insert(SensorData sensorData);

    SensorData findLatest();
}
