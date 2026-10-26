package com.example.javatest.mapper;

import com.example.javatest.entity.SensorData;
import org.apache.ibatis.annotations.Mapper;
import org.apache.ibatis.annotations.Param;

import java.util.List;

@Mapper
public interface SensorDataMapper {

    int insert(SensorData sensorData);

    SensorData findLatest();

    List<SensorData> findHistory(@Param("limit") int limit);
}
