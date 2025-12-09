#include "XdsMonitor.h"
#include <thread>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <vector>

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

void XdsMonitor::onDataReceived(SimpleBLE::ByteArray bytes) {
    m_lastDataTime = millis();
    const uint8_t* data = (const uint8_t*)bytes.c_str();
    int len = bytes.length();

    if (len < 11) return;

    // 解析逻辑 - 与 Arduino 固件完全对齐
    uint16_t totalPower = getUnsignedValue(data, 0);
    int16_t leftPower = getSignedValue(data, 2);
    int16_t rightPower = getSignedValue(data, 4);
    int16_t angle = 0;
    if (len >= 8) {
        angle = getSignedValue(data, 6); // 新增：角度解析 (Byte 6-7)
    }
    uint16_t cadence = getUnsignedValue(data, 8);
    uint8_t errorCode = data[10];

    // 简单过滤
    if (totalPower > 2500) return;

    std::lock_guard<std::mutex> lock(m_printMutex);

    // 使用 ANSI 控制码清空当前行并覆盖
    // 注意：增加换行以便观察数据流，或者使用回车覆盖
    // 这里我改为每行输出，因为包含了 Hex 数据，单行刷新可能显示不全
    // 如果你喜欢单行刷新，可以保留 \r

    std::cout << "\r\033[K"; // 清除行

    // 格式化输出
    std::cout << "PWR:" << std::left << std::setw(4) << totalPower
        << " CAD:" << std::setw(3) << cadence
        << " L/R:" << std::setw(4) << leftPower << "/" << std::setw(4) << rightPower
        << " Ang:" << std::setw(4) << angle
        << " Err:" << (int)errorCode << " ";

    // 简略显示 HEX (最后几个字节)
    // printHex(data, len); 

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
    // 如果已经有目标地址且开启自动重连，尝试直接跳过扫描（SimpleBLE 暂不支持直接按地址连接未扫描设备，所以仍需扫描）
    // 但我们可以自动匹配

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

        // 如果我们之前连接过，尝试自动重连
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
                m_targetAddress = m_targetDevice.address(); // 记住地址以便重连
                isTargetSelected = true;
                m_autoReconnect = true; // 开启后续自动重连
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

    // 重置 UUID
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
        std::cerr << "[错误] 未找到服务或特征值！可能不是兼容的 XDS 设备。" << std::endl;
        m_targetDevice.disconnect();
        return false;
    }
    return true;
}

void XdsMonitor::startMonitoring() {
    std::cout << "\n[3/3] 开始监控数据 (按 Ctrl+C 退出)..." << std::endl;
    std::cout << "格式说明: PWR(总功率) CAD(踏频) L/R(左右腿数据) Ang(角度) Err(错误码)" << std::endl;

    try {
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

        // 检查连接状态
        if (!m_targetDevice.is_connected()) {
            std::cout << "\n[警告] 设备连接断开！" << std::endl;
            break; // 跳出循环，触发外部重连逻辑
        }

        // 检查数据超时
        if (millis() - m_lastDataTime > 5000) {
            std::lock_guard<std::mutex> lock(m_printMutex);
            std::cout << "\r\033[K" << "[状态] 等待数据中..." << std::flush;
            m_lastDataTime = millis(); // 重置以免疯狂打印
        }
    }

    // 清理工作
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

    // 外层循环：负责整体流程（扫描 -> 连接 -> 监控 -> 断开 -> 重试）
    while (s_running) {
        // 1. 扫描并选择 (如果是断线重连，scanAndSelectDevice 内部会尝试匹配上次的 MAC)
        if (!scanAndSelectDevice()) break;

        // 2. 连接
        if (!s_running) break;
        if (connectDevice()) {
            // 3. 监控 (阻塞直到断开或手动退出)
            startMonitoring();
        }
        else {
            std::cout << "[系统] 连接失败，3秒后重试..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // 如果用户按了 Ctrl+C，就不再重试
        if (!s_running) break;

        std::cout << "\n[系统] 准备重新建立连接..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[系统] 程序已退出。" << std::endl;
}