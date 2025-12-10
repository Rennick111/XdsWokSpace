#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
#include <simpleble/SimpleBLE.h>

class XdsMonitor {
public:
    XdsMonitor();

    // 主运行函数
    void run();

    // 静态方法用于停止运行 (供信号处理调用)
    static void stop();

private:
    // 内部逻辑步骤
    bool initAdapter();
    bool scanAndSelectDevice(); // 返回 true 表示选到了设备
    bool connectDevice();
    void startMonitoring();

    // 辅助工具
    long long millis();
    uint16_t getUnsignedValue(const uint8_t* data, int offset);
    int16_t getSignedValue(const uint8_t* data, int offset);
    void printHex(const uint8_t* data, int length);

    // 数据回调
    void onDataReceived(SimpleBLE::ByteArray bytes);

private:
    // 成员变量
    SimpleBLE::Adapter m_adapter;
    SimpleBLE::Peripheral m_targetDevice;

    // 状态控制
    static std::atomic<bool> s_running;
    std::atomic<long long> m_lastDataTime{ 0 };
    std::mutex m_printMutex;

    // 选中的设备地址，用于重连
    std::string m_targetAddress;
    bool m_autoReconnect = false;

    // 配置常量
    const std::string TARGET_UUID_SUBSTRING = "1828";
    const std::string CHAR_UUID_SUBSTRING = "2a63";

    // 运行时发现的 UUID
    std::string m_foundServiceUUID;
    std::string m_foundCharUUID;

    // --- 新增：统计与计算变量 ---

    // 1. 时间统计
    long long m_startTime = 0;           // 开始监控的时间戳 (ms)

    // 2. 功率统计
    uint16_t m_maxPower = 0;             // 最大功率 (W)
    unsigned long long m_sumPower = 0;   // 功率累加和 (用于算平均)
    uint32_t m_powerSampleCount = 0;     // 功率样本计数

    // 3. 踏频统计 (包含累计值转RPM算法)
    uint16_t m_lastCrankCount = 0;       // 上一次的累计圈数
    long long m_lastCrankTime = 0;       // 上一次收到圈数的时间
    bool m_firstPacket = true;           // 是否是第一个包

    unsigned long long m_sumCadence = 0; // 踏频累加和 (RPM)
    uint32_t m_cadenceSampleCount = 0;   // 踏频样本计数
};