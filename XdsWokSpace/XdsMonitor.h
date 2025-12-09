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
    void printHex(const uint8_t* data, int length); // 新增：打印原始数据

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
};