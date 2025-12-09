#include "XdsMonitor.h"
#include <thread>
#include <iomanip>
#include <chrono>
#include <algorithm>

// 初始化静态成员
std::atomic<bool> XdsMonitor::s_running{ true };

XdsMonitor::XdsMonitor() {
    // 构造函数可用于初始化
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
    return (int16_t)getUnsignedValue(data, offset);
}

void XdsMonitor::onDataReceived(SimpleBLE::ByteArray bytes) {
    m_lastDataTime = millis();
    const uint8_t* data = (const uint8_t*)bytes.c_str();

    if (bytes.length() < 11) return;

    uint16_t totalPower = getUnsignedValue(data, 0);
    int16_t leftPower = getSignedValue(data, 2);
    int16_t rightPower = getSignedValue(data, 4);
    uint16_t cadence = getUnsignedValue(data, 8);
    uint8_t errorCode = data[10];

    if (totalPower > 2000) return;

    std::lock_guard<std::mutex> lock(m_printMutex);
    std::cout << "\r\033[K"
        << "⚡ 功率: " << std::left << std::setw(4) << totalPower << " W | "
        << "🔄 踏频: " << std::setw(4) << cadence << " RPM | "
        << "⚖️  左/右: " << std::setw(4) << leftPower << " / " << std::setw(4) << rightPower << " | "
        << "Err: " << (int)errorCode
        << std::flush;
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

void XdsMonitor::scanAndSelectDevice() {
    bool isTargetSelected = false;

    while (s_running && !isTargetSelected) {
        std::cout << "\n[1/3] 正在扫描周围设备 (5秒)..." << std::endl;
        m_adapter.scan_for(5000);
        auto peripherals = m_adapter.scan_get_results();

        if (peripherals.empty()) {
            std::cout << "[警告] 未发现任何设备。重试中..." << std::endl;
            continue;
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

        std::cout << "请输入设备 ID (r 重试): ";
        if (recommendedIndex != -1) std::cout << "[推荐: " << recommendedIndex << "] ";

        std::string input;
        std::cin >> input;
        if (input == "r" || input == "R") continue;

        try {
            int selectedId = std::stoi(input);
            if (selectedId >= 0 && selectedId < peripherals.size()) {
                m_targetDevice = peripherals[selectedId];
                isTargetSelected = true;
            }
        }
        catch (...) { std::cout << "输入无效" << std::endl; }
    }
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

    if (m_foundServiceUUID.empty() || m_foundCharUUID.empty()) {
        std::cerr << "[错误] 未找到服务或特征值！" << std::endl;
        m_targetDevice.disconnect();
        return false;
    }
    return true;
}

void XdsMonitor::startMonitoring() {
    std::cout << "\n[3/3] 开始监控数据 (按 Ctrl+C 退出)..." << std::endl;
    try {
        // 使用 lambda 绑定成员函数
        m_targetDevice.notify(m_foundServiceUUID, m_foundCharUUID, [this](SimpleBLE::ByteArray bytes) {
            this->onDataReceived(bytes);
            });
        m_lastDataTime = millis();
    }
    catch (std::exception& e) {
        std::cerr << "[错误] 订阅失败: " << e.what() << std::endl;
        return;
    }

    while (s_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!m_targetDevice.is_connected()) break;
        if (millis() - m_lastDataTime > 5000) {
            std::lock_guard<std::mutex> lock(m_printMutex);
            std::cout << "\r\033[K" << "[状态] 无数据..." << std::flush;
        }
    }

    if (m_targetDevice.is_connected()) {
        try {
            m_targetDevice.unsubscribe(m_foundServiceUUID, m_foundCharUUID);
            m_targetDevice.disconnect();
        }
        catch (...) {}
    }
    std::cout << "\n[系统] 已断开。" << std::endl;
}

void XdsMonitor::run() {
    if (!initAdapter()) return;
    scanAndSelectDevice();
    if (!s_running) return;
    if (connectDevice()) {
        startMonitoring();
    }
}