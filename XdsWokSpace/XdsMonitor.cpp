#include "XdsMonitor.h"
#include <thread>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <vector>
#include <cmath> // 用于 abs

// 初始化静态成员
std::atomic<bool> XdsMonitor::s_running{ true };

XdsMonitor::XdsMonitor() {
}

void XdsMonitor::stop() {
    s_running = false;
}

long long XdsMonitor::millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

uint16_t XdsMonitor::getUnsignedValue(const uint8_t* data, int offset) {
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

int16_t XdsMonitor::getSignedValue(const uint8_t* data, int offset) {
    // 转换为有符号，处理补码
    uint16_t val = getUnsignedValue(data, offset);
    return (int16_t)val;
}

void XdsMonitor::printHex(const uint8_t* data, int length) {
    std::cout << "[RAW] ";
    for (int i = 0; i < length; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
    }
    std::cout << std::dec << std::setfill(' '); // 恢复十进制
}

// 核心数据处理函数
void XdsMonitor::onDataReceived(SimpleBLE::ByteArray bytes) {
    long long now = millis();
    m_lastDataTime = now;

    const uint8_t* data = (const uint8_t*)bytes.c_str();
    int len = bytes.length();

    if (len < 11) return;

    // ---------------------------------------------------------
    // 1. 基础数据解析 (Raw Parsing)
    // ---------------------------------------------------------
    uint16_t instPower = getUnsignedValue(data, 0);       // 实时功率
    int16_t leftPower = getSignedValue(data, 2);         // 左腿功率
    int16_t rightPower = getSignedValue(data, 4);         // 右腿功率
    int16_t angle = 0;
    if (len >= 8) {
        angle = getSignedValue(data, 6);                  // 角度
    }

    // 获取 Byte 8-9 的值 (累计圈数)
    uint16_t rawCadenceData = getUnsignedValue(data, 8);
    uint8_t errorCode = data[10];

    // 简单过滤异常值
    if (instPower > 2500) return;

    // ---------------------------------------------------------
    // 2. 踏频计算 (累计圈数 -> 实时 RPM)
    // ---------------------------------------------------------
    uint16_t instCadence = 0; // 最终计算出的实时 RPM

    if (m_firstPacket) {
        m_lastCrankCount = rawCadenceData;
        m_lastCrankTime = now;
        m_firstPacket = false;
        instCadence = 0;
    }
    else {
        // 计算圈数增量 (处理 uint16 65535->0 的溢出)
        uint16_t diffCount = 0;
        if (rawCadenceData >= m_lastCrankCount) {
            diffCount = rawCadenceData - m_lastCrankCount;
        }
        else {
            diffCount = (65535 - m_lastCrankCount) + rawCadenceData + 1;
        }

        long long diffTime = now - m_lastCrankTime;

        // 只有当圈数发生变化，或者时间过去太久(停止踩踏)，才更新 RPM
        if (diffCount > 0 && diffTime > 0) {
            // RPM = (圈数 * 60000ms) / 时间ms
            instCadence = (uint16_t)((diffCount * 60000) / diffTime);

            // 限制一下最大值，防止数据抖动出现 300+ rpm
            if (instCadence > 200) instCadence = 200;

            // 更新状态
            m_lastCrankCount = rawCadenceData;
            m_lastCrankTime = now;
        }
        else {
            // 如果圈数没变，且距离上次更新超过 2.5秒 (相当于 < 24 RPM)，认为停止
            if (now - m_lastCrankTime > 2500) {
                instCadence = 0;
            }
            else {
                // 暂时保持 0 或上一次的值？通常 App 会归零
                instCadence = 0;
            }
        }
    }

    // ---------------------------------------------------------
    // 3. App 衍生数据计算 (Statistics)
    // ---------------------------------------------------------

    // A. 骑行时间 (MM:SS)
    long long elapsedMs = now - m_startTime;
    int totalSeconds = elapsedMs / 1000;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;

    // B. 最大功率 (Max Power)
    if (instPower > m_maxPower) {
        m_maxPower = instPower;
    }

    // C. 平均功率 (Avg Power) - 包含 0 值以反映真实强度
    m_sumPower += instPower;
    m_powerSampleCount++;
    int avgPower = (m_powerSampleCount > 0) ? (m_sumPower / m_powerSampleCount) : 0;

    // D. 平均踏频 (Avg Cadence) - 通常只统计踩踏期间 (Non-zero averaging)
    if (instCadence > 0) {
        m_sumCadence += instCadence;
        m_cadenceSampleCount++;
    }
    int avgCadence = (m_cadenceSampleCount > 0) ? (m_sumCadence / m_cadenceSampleCount) : 0;

    // E. 左右平衡率 (L/R Balance)
    // 公式: 左 / (左 + 右) * 100%
    int leftBalance = 50;
    int rightBalance = 50;
    int totalLR = std::abs(leftPower) + std::abs(rightPower); // 使用绝对值防止负数干扰

    if (totalLR > 0) {
        leftBalance = (std::abs(leftPower) * 100) / totalLR;
        rightBalance = 100 - leftBalance;
    }

    // ---------------------------------------------------------
    // 4. 格式化输出 (UI Display)
    // ---------------------------------------------------------
    std::lock_guard<std::mutex> lock(m_printMutex);

    // 使用 ANSI 控制码清除当前行
    std::cout << "\r\033[K";

    // 打印格式：[时间] 功率(实/均/最) | 踏频(实/均) | 平衡(L/R) | 角度
    std::cout << "[" << std::setw(2) << std::setfill('0') << minutes << ":"
        << std::setw(2) << std::setfill('0') << seconds << "] "
        << std::setfill(' ') // 恢复填充为空格

        << "PWR:" << std::setw(3) << instPower << "/"
        << std::setw(3) << avgPower << "/"
        << std::setw(3) << m_maxPower << "W "

        << "CAD:" << std::setw(3) << instCadence << "/"
        << std::setw(3) << avgCadence << " "

        << "L/R:" << std::setw(2) << leftBalance << "/"
        << std::setw(2) << rightBalance << "% "

        << "Ang:" << std::setw(3) << angle << " "
        << "E:" << (int)errorCode;

    std::cout << std::flush;
}

bool XdsMonitor::initAdapter() {
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "[错误] 未检测到蓝牙适配器！" << std::endl;
        return false;
    }
    m_adapter = adapters[0];
    std::cout << "[系统] 使用适配器: " << m_adapter.identifier() << " [" << m_adapter.address() << "]" << std::endl;
    return true;
}

bool XdsMonitor::scanAndSelectDevice() {
    bool isTargetSelected = false;
    m_autoReconnect = false;

    while (s_running && !isTargetSelected) {
        std::cout << "\n[1/3] 正在扫描周围设备 (5秒)..." << std::endl;
        m_adapter.scan_for(5000);
        auto peripherals = m_adapter.scan_get_results();

        if (peripherals.empty()) {
            std::cout << "[警告] 未发现任何设备。重试中..." << std::endl;
            continue;
        }

        // 自动重连逻辑
        if (!m_targetAddress.empty()) {
            for (auto& p : peripherals) {
                if (p.address() == m_targetAddress) {
                    m_targetDevice = p;
                    std::cout << "[系统] 自动找到上次连接的设备: " << p.identifier() << std::endl;
                    return true;
                }
            }
        }

        std::cout << "\n---------------------------------------------------------------" << std::endl;
        std::cout << " ID | 设备名称 (Identifier) | MAC 地址          | 信号 | UUIDs" << std::endl;
        std::cout << "---------------------------------------------------------------" << std::endl;

        int index = 0;
        int recommendedIndex = -1;

        for (auto& p : peripherals) {
            std::string name = p.identifier();
            std::string uuidListStr = "";
            bool isCandidate = false;

            for (auto& service : p.services()) {
                if (service.uuid().find(TARGET_UUID_SUBSTRING) != std::string::npos) isCandidate = true;
                uuidListStr += service.uuid().substr(0, 8) + ".. ";
            }
            if (name.find("XDS") != std::string::npos || name.find("Power") != std::string::npos) isCandidate = true;

            std::cout << (isCandidate ? "⭐" : "  ")
                << "[" << index << "] "
                << std::left << std::setw(20) << (name.empty() ? "<无名称>" : name) << " | "
                << p.address() << " | "
                << std::setw(4) << p.rssi() << " | " << uuidListStr << std::endl;

            if (isCandidate && recommendedIndex == -1) recommendedIndex = index;
            index++;
        }
        std::cout << "---------------------------------------------------------------" << std::endl;

        std::cout << "请输入设备 ID (r 重试, q 退出): ";
        if (recommendedIndex != -1) std::cout << "[推荐: " << recommendedIndex << "] ";

        std::string input;
        std::cin >> input;
        if (input == "r" || input == "R") continue;
        if (input == "q" || input == "Q") { stop(); return false; }

        try {
            int selectedId = -1;
            if (recommendedIndex != -1 && input.empty()) {
                selectedId = recommendedIndex;
            }
            else {
                selectedId = std::stoi(input);
            }

            if (selectedId >= 0 && selectedId < peripherals.size()) {
                m_targetDevice = peripherals[selectedId];
                m_targetAddress = m_targetDevice.address();
                isTargetSelected = true;
                m_autoReconnect = true;
            }
        }
        catch (...) { std::cout << "输入无效" << std::endl; }
    }
    return isTargetSelected;
}

bool XdsMonitor::connectDevice() {
    std::cout << "\n[2/3] 正在连接 [" << m_targetDevice.identifier() << "]..." << std::endl;
    try {
        m_targetDevice.connect();
    }
    catch (std::exception& e) {
        std::cerr << "[错误] 连接失败: " << e.what() << std::endl;
        return false;
    }

    std::cout << "[系统] 正在寻找 XDS 服务..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    m_foundServiceUUID = "";
    m_foundCharUUID = "";

    try {
        for (auto& service : m_targetDevice.services()) {
            if (service.uuid().find(TARGET_UUID_SUBSTRING) != std::string::npos) {
                m_foundServiceUUID = service.uuid();
                for (auto& characteristic : service.characteristics()) {
                    if (characteristic.uuid().find(CHAR_UUID_SUBSTRING) != std::string::npos) {
                        m_foundCharUUID = characteristic.uuid();
                        break;
                    }
                }
            }
            if (!m_foundServiceUUID.empty() && !m_foundCharUUID.empty()) break;
        }
    }
    catch (...) {
        std::cerr << "[错误] 服务发现异常" << std::endl;
        return false;
    }

    if (m_foundServiceUUID.empty() || m_foundCharUUID.empty()) {
        std::cerr << "[错误] 未找到服务或特征值！" << std::endl;
        m_targetDevice.disconnect();
        return false;
    }
    return true;
}

void XdsMonitor::startMonitoring() {
    std::cout << "\n[3/3] 开始监控数据 (按 Ctrl+C 退出)..." << std::endl;
    std::cout << "==========================================================================" << std::endl;
    std::cout << "显示格式: [时间] PWR:实时/平均/最大 | CAD:实时/平均 | L/R:平衡% | Ang:角度" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    try {
        m_targetDevice.notify(m_foundServiceUUID, m_foundCharUUID, [this](SimpleBLE::ByteArray bytes) {
            this->onDataReceived(bytes);
            });

        // --- 初始化统计数据 ---
        m_lastDataTime = millis();
        m_startTime = millis(); // 记录开始时间

        m_maxPower = 0;
        m_sumPower = 0;
        m_powerSampleCount = 0;

        m_sumCadence = 0;
        m_cadenceSampleCount = 0;

        m_lastCrankCount = 0;
        m_firstPacket = true;
        // --------------------
    }
    catch (std::exception& e) {
        std::cerr << "[错误] 订阅失败: " << e.what() << std::endl;
        return;
    }

    while (s_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!m_targetDevice.is_connected()) {
            std::cout << "\n[警告] 设备连接断开！" << std::endl;
            break;
        }

        if (millis() - m_lastDataTime > 5000) {
            std::lock_guard<std::mutex> lock(m_printMutex);
            std::cout << "\r\033[K" << "[状态] 等待数据中..." << std::flush;
            m_lastDataTime = millis();
        }
    }

    if (m_targetDevice.is_connected()) {
        try {
            m_targetDevice.unsubscribe(m_foundServiceUUID, m_foundCharUUID);
            m_targetDevice.disconnect();
        }
        catch (...) {}
    }
}

void XdsMonitor::run() {
    if (!initAdapter()) return;

    while (s_running) {
        if (!scanAndSelectDevice()) break;

        if (!s_running) break;
        if (connectDevice()) {
            startMonitoring();
        }
        else {
            std::cout << "[系统] 连接失败，3秒后重试..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        if (!s_running) break;

        std::cout << "\n[系统] 准备重新建立连接..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[系统] 程序已退出。" << std::endl;
}