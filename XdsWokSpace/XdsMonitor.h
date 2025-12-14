#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
#include <chrono> // 引入 chrono 用于高精度踏频计算
#include <simpleble/SimpleBLE.h>

// 定义设备类型枚举
enum class DeviceType {
    UNKNOWN,
    XDS_POWER,      // 原有的 XDS 功率计
    STD_POWER,      // 标准蓝牙功率计 (CPS)
    HEART_RATE,     // 蓝牙心率带 (HRS)
    CSC_SENSOR      // 速度/踏频传感器 (CSCP)
};

class XdsMonitor {
public:
    XdsMonitor();

    // 主运行函数
    void run();

    // 静态方法用于停止运行
    static void stop();

private:
    // --- 内部流程 ---
    bool initAdapter();
    bool scanAndSelectDevice();
    bool connectDevice();
    void startMonitoring();

    // --- 辅助工具 ---
    long long millis();
    uint16_t getUnsignedValue(const uint8_t* data, int offset);
    int16_t getSignedValue(const uint8_t* data, int offset);

    // --- 数据解析路由 ---
    void onDataReceived(SimpleBLE::ByteArray bytes);

    // --- 各类设备的具体解析逻辑 ---
    void parseXdsData(const uint8_t* data, int len);       // XDS 解析 (V2.1 算法)
    void parseStdPowerData(const uint8_t* data, int len);  // 标准功率计解析
    void parseHeartRateData(const uint8_t* data, int len); // 心率解析
    void parseCscData(const uint8_t* data, int len);       // 踏频解析

    // --- UI 显示 ---
    void refreshDisplay();

private:
    SimpleBLE::Adapter m_adapter;
    SimpleBLE::Peripheral m_targetDevice;

    static std::atomic<bool> s_running;
    std::atomic<long long> m_lastDataTime{ 0 };
    std::mutex m_printMutex;

    std::string m_targetAddress;
    bool m_autoReconnect = false;

    // --- 当前连接的设备类型 ---
    DeviceType m_currentType = DeviceType::UNKNOWN;

    // --- 发现的服务与特征 UUID ---
    std::string m_targetServiceUUID;
    std::string m_targetCharUUID;

    // --- 统计与显示数据 (通用存储) ---
    long long m_startTime = 0;

    // 实时数据缓存
    int m_displayPower = 0;
    int m_displayCadence = 0;
    int m_displayHeartRate = 0;
    int m_displayLBalance = 0;
    int m_displayRBalance = 0;
    int m_displayAngle = 0;

    // 统计用
    uint16_t m_maxPower = 0;
    unsigned long long m_sumPower = 0;
    uint32_t m_powerSampleCount = 0;
    unsigned long long m_sumCadence = 0;
    uint32_t m_cadenceSampleCount = 0;

    // [修改] 踏频计算辅助 (适配 XDS_Monitor.cpp V2.1 算法)
    uint16_t m_prev_revs = 0;                              // 上一次的累计转数
    std::chrono::steady_clock::time_point m_prev_time;     // 上一次的时间点
    bool m_first_calc = true;                              // 是否首次计算标记

    // [修复] 兼容标准 CSCP 的旧变量保留 (补回 m_firstPacket)
    uint16_t m_lastCrankCount = 0;
    long long m_lastCrankTime = 0;
    bool m_firstPacket = true;                             
};