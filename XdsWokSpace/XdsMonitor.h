#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
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
    void parseXdsData(const uint8_t* data, int len);       // 原 XDS 解析
    void parseStdPowerData(const uint8_t* data, int len);  // 标准功率计解析 (UUID 2A63)
    void parseHeartRateData(const uint8_t* data, int len); // 心率解析 (UUID 2A37)
    void parseCscData(const uint8_t* data, int len);       // 踏频解析 (UUID 2A5B)

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

    // 踏频计算辅助 (用于标准 CSCP 和 XDS)
    uint16_t m_lastCrankCount = 0;
    long long m_lastCrankTime = 0;
    bool m_firstPacket = true;
};