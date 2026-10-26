package com.example.javatest.service.impl;

import com.example.javatest.entity.SensorData;
import com.example.javatest.mapper.SensorDataMapper;
import com.example.javatest.service.SensorDataService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;

import java.util.List;

@Service
public class SensorDataServiceImpl implements SensorDataService {

    @Autowired
    private SensorDataMapper sensorDataMapper;

    @Override
    public void save(SensorData sensorData) {
        sensorDataMapper.insert(sensorData);
    }

    @Override
    public SensorData latest() {
        return sensorDataMapper.findLatest();
    }

    @Override
    public List<SensorData> history(int limit) {
        return sensorDataMapper.findHistory(limit);
    }
}
