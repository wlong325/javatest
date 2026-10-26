package com.example.javatest.service;

import com.example.javatest.entity.SensorData;

public interface SensorDataService {

    void save(SensorData sensorData);

    SensorData latest();
}
