package com.example.javatest.controller;

import com.example.javatest.entity.SensorData;
import com.example.javatest.service.SensorDataService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.*;

import java.util.List;

@RestController
@RequestMapping("/api/sensor")
public class SensorController {

    @Autowired
    private SensorDataService sensorDataService;

    @PostMapping
    public String save(@RequestBody SensorData sensorData) {
        sensorDataService.save(sensorData);
        return "接收成功";
    }

    @GetMapping("/latest")
    public SensorData latest() {
        return sensorDataService.latest();
    }

    @GetMapping("/history")
    public List<SensorData> history(@RequestParam(defaultValue = "100") int limit) {
        return sensorDataService.history(limit);
    }
}
