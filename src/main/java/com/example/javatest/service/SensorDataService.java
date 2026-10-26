package com.example.javatest.service;

import com.example.javatest.entity.SensorData;

import java.util.List;

public interface SensorDataService {

    void save(SensorData sensorData);

    SensorData latest();

    List<SensorData> history(int limit);
}
