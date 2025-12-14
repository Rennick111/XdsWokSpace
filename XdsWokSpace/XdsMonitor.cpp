#include "XdsMonitor.h"
#include <thread>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <sstream>

// --- 标准蓝牙 UUID 定义 ---
const std::string UUID_SVC_XDS = "1828"; // XDS 自定义服务
const std::string UUID_SVC_CPS = "1818"; // Cycling Power Service (标准功率)
const std::string UUID_SVC_HRS = "180d"; // Heart Rate Service (心率)
const std::string UUID_SVC_CSCP = "1816"; // Cycling Speed and Cadence (踏频)

const std::string UUID_CHR_XDS_DATA = "2a63"; // XDS 数据
const std::string UUID_CHR_CPS_MEAS = "2a63"; // Standard Power Measurement
const std::string UUID_CHR_HRS_MEAS = "2a37"; // Heart Rate Measurement
const std::string UUID_CHR_CSC_MEAS = "2a5b"; // CSC Measurement

std::atomic<bool> XdsMonitor::s_running{ true };

XdsMonitor::XdsMonitor() {}

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
    uint16_t val = getUnsignedValue(data, offset);
    return (int16_t)val;
}

// ---------------------------------------------------------
// 核心：扫描与设备识别逻辑
// ---------------------------------------------------------
bool XdsMonitor::scanAndSelectDevice() {
    bool isTargetSelected = false;
    m_autoReconnect = false;

    while (s_running && !isTargetSelected) {
        std::cout << "\n[1/3] 正在扫描周围设备 (支持: XDS/标准功率计/心率带/踏频器)..." << std::endl;
        m_adapter.scan_for(4000);
        auto peripherals = m_adapter.scan_get_results();

        if (peripherals.empty()) {
            std::cout << "[警告] 未发现设备，重试中..." << std::endl;
            continue;
        }

        // 自动重连逻辑
        if (!m_targetAddress.empty()) {
            for (auto& p : peripherals) {
                if (p.address() == m_targetAddress) {
                    m_targetDevice = p;
                    std::cout << "[系统] 自动重连设备: " << p.identifier() << std::endl;
                    return true; // 类型保持上次的不变
                }
            }
        }

        std::cout << "\n-------------------------------------------------------------------------------" << std::endl;
        std::cout << " ID | 设备名称             | 类型(推测)   | 信号 | MAC 地址" << std::endl;
        std::cout << "-------------------------------------------------------------------------------" << std::endl;

        int index = 0;
        struct Candidate { int id; DeviceType type; };
        std::vector<Candidate> candidates;

        for (auto& p : peripherals) {
            std::string name = p.identifier();
            if (name.empty()) name = "<Unknown>";

            DeviceType detectedType = DeviceType::UNKNOWN;
            std::string typeStr = "未知";

            // 根据 UUID 判断设备类型
            for (auto& service : p.services()) {
                std::string uuid = service.uuid();
                if (uuid.find(UUID_SVC_XDS) != std::string::npos) {
                    detectedType = DeviceType::XDS_POWER; typeStr = "XDS 功率计"; break;
                }
                if (uuid.find(UUID_SVC_CPS) != std::string::npos) {
                    detectedType = DeviceType::STD_POWER; typeStr = "标准功率计"; break;
                }
                if (uuid.find(UUID_SVC_HRS) != std::string::npos) {
                    detectedType = DeviceType::HEART_RATE; typeStr = "心率带"; break;
                }
                if (uuid.find(UUID_SVC_CSCP) != std::string::npos) {
                    detectedType = DeviceType::CSC_SENSOR; typeStr = "踏频传感器"; break;
                }
            }

            // 名字辅助判断
            if (detectedType == DeviceType::UNKNOWN) {
                if (name.find("XDS") != std::string::npos) { detectedType = DeviceType::XDS_POWER; typeStr = "XDS (Name)"; }
            }

            // 打印列表
            std::cout << (detectedType != DeviceType::UNKNOWN ? "⭐" : "  ")
                << "[" << index << "] "
                << std::left << std::setw(20) << name.substr(0, 20) << " | "
                << std::setw(12) << typeStr << " | "
                << std::setw(4) << p.rssi() << " | " << p.address() << std::endl;

            if (detectedType != DeviceType::UNKNOWN) {
                candidates.push_back({ index, detectedType });
            }
            index++;
        }
        std::cout << "-------------------------------------------------------------------------------" << std::endl;

        std::cout << "输入 ID 连接 (r=刷新, q=退出): ";
        std::string input;
        std::cin >> input;

        if (input == "q" || input == "Q") { stop(); return false; }
        if (input == "r" || input == "R") continue;

        try {
            int sel = std::stoi(input);
            if (sel >= 0 && sel < (int)peripherals.size()) {
                m_targetDevice = peripherals[sel];
                m_targetAddress = m_targetDevice.address();

                // 确定选中的设备类型
                m_currentType = DeviceType::UNKNOWN;
                // 优先从候选列表中找类型，如果不在候选列表（未知设备），则再次尝试扫描服务
                for (auto& c : candidates) {
                    if (c.id == sel) m_currentType = c.type;
                }

                // 如果 UI 上没识别出来，但用户强行连了，连接阶段会二次确认
                isTargetSelected = true;
                m_autoReconnect = true;
            }
        }
        catch (...) {}
    }
    return isTargetSelected;
}

// ---------------------------------------------------------
// 连接与服务匹配
// ---------------------------------------------------------
bool XdsMonitor::connectDevice() {
    std::cout << "\n[2/3] 正在连接 [" << m_targetDevice.identifier() << "]..." << std::endl;
    try {
        m_targetDevice.connect();
    }
    catch (std::exception& e) {
        std::cerr << "连接失败: " << e.what() << std::endl;
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::cout << "[系统] 正在匹配服务协议..." << std::endl;

    // 如果扫描阶段没确认类型，这里再遍历一次服务
    if (m_currentType == DeviceType::UNKNOWN) {
        for (auto& service : m_targetDevice.services()) {
            if (service.uuid().find(UUID_SVC_XDS) != std::string::npos) m_currentType = DeviceType::XDS_POWER;
            else if (service.uuid().find(UUID_SVC_CPS) != std::string::npos) m_currentType = DeviceType::STD_POWER;
            else if (service.uuid().find(UUID_SVC_HRS) != std::string::npos) m_currentType = DeviceType::HEART_RATE;
            else if (service.uuid().find(UUID_SVC_CSCP) != std::string::npos) m_currentType = DeviceType::CSC_SENSOR;
        }
    }

    // 根据类型设定目标 UUID
    m_targetServiceUUID = "";
    m_targetCharUUID = "";

    switch (m_currentType) {
    case DeviceType::XDS_POWER:
        m_targetServiceUUID = UUID_SVC_XDS; m_targetCharUUID = UUID_CHR_XDS_DATA;
        std::cout << ">> 识别模式: XDS 专用协议" << std::endl;
        break;
    case DeviceType::STD_POWER:
        m_targetServiceUUID = UUID_SVC_CPS; m_targetCharUUID = UUID_CHR_CPS_MEAS;
        std::cout << ">> 识别模式: 标准蓝牙功率 (CPS)" << std::endl;
        break;
    case DeviceType::HEART_RATE:
        m_targetServiceUUID = UUID_SVC_HRS; m_targetCharUUID = UUID_CHR_HRS_MEAS;
        std::cout << ">> 识别模式: 蓝牙心率 (HRS)" << std::endl;
        break;
    case DeviceType::CSC_SENSOR:
        m_targetServiceUUID = UUID_SVC_CSCP; m_targetCharUUID = UUID_CHR_CSC_MEAS;
        std::cout << ">> 识别模式: 踏频传感器 (CSCP)" << std::endl;
        break;
    default:
        std::cerr << "[错误] 无法识别该设备类型或不支持的协议！" << std::endl;
        m_targetDevice.disconnect();
        return false;
    }

    // 真正的 UUID 查找 (处理 UUID 大小写或完整性差异)
    bool found = false;
    for (auto& service : m_targetDevice.services()) {
        if (service.uuid().find(m_targetServiceUUID) != std::string::npos) {
            auto realServiceUUID = service.uuid();
            for (auto& ch : service.characteristics()) {
                if (ch.uuid().find(m_targetCharUUID) != std::string::npos) {
                    m_targetServiceUUID = realServiceUUID;
                    m_targetCharUUID = ch.uuid();
                    found = true;
                    break;
                }
            }
        }
        if (found) break;
    }

    if (!found) {
        std::cerr << "[错误] 未找到对应特征值，设备可能不兼容。" << std::endl;
        m_targetDevice.disconnect();
        return false;
    }
    return true;
}

// ---------------------------------------------------------
// 数据分发入口
// ---------------------------------------------------------
void XdsMonitor::onDataReceived(SimpleBLE::ByteArray bytes) {
    long long now = millis();
    m_lastDataTime = now;

    // 使用 data() 获取指针
    const uint8_t* data = (const uint8_t*)bytes.data();
    int len = (int)bytes.length();

    // 路由到具体的解析器
    switch (m_currentType) {
    case DeviceType::XDS_POWER:  parseXdsData(data, len); break;
    case DeviceType::STD_POWER:  parseStdPowerData(data, len); break;
    case DeviceType::HEART_RATE: parseHeartRateData(data, len); break;
    case DeviceType::CSC_SENSOR: parseCscData(data, len); break;
    }

    refreshDisplay();
}

// ---------------------------------------------------------
// 1. XDS 协议解析 (V2.1 算法)
// ---------------------------------------------------------
void XdsMonitor::parseXdsData(const uint8_t* data, int len) {
    if (len < 10) return;

    // --- A. 解析基础数据 ---
    uint16_t instPower = getUnsignedValue(data, 0);
    int16_t leftPower = getSignedValue(data, 2);
    int16_t rightPower = getSignedValue(data, 4);
    int16_t angle = getSignedValue(data, 6);
    uint16_t current_revs = getUnsignedValue(data, 8); // 累计转数

    if (instPower > 3000) instPower = 0;

    // --- B. 踏频计算算法 ---
    auto now = std::chrono::steady_clock::now();
    double currentRpm = 0.0;

    if (m_first_calc) {
        m_prev_revs = current_revs;
        m_prev_time = now;
        m_first_calc = false;
    }
    else {
        uint16_t rev_diff = current_revs - m_prev_revs;
        if (rev_diff > 0) {
            std::chrono::duration<double> time_diff = now - m_prev_time;
            double seconds = time_diff.count();

            if (seconds > 0) {
                double rpm = (rev_diff / seconds) * 60.0;
                if (rpm < 250) {
                    currentRpm = rpm;
                }
            }
            m_prev_revs = current_revs;
            m_prev_time = now;
        }
        else {
            std::chrono::duration<double> idle_time = now - m_prev_time;
            if (idle_time.count() > 2.5) {
                currentRpm = 0.0;
            }
            else {
                currentRpm = m_displayCadence;
            }
        }
    }

    // --- C. 更新显示缓存 ---
    m_displayPower = instPower;
    m_displayAngle = angle;
    m_displayCadence = (int)currentRpm;

    // 左右平衡计算
    int totalLR = std::abs(leftPower) + std::abs(rightPower);
    if (totalLR > 0) {
        m_displayLBalance = (std::abs(leftPower) * 100) / totalLR;
        m_displayRBalance = 100 - m_displayLBalance;
    }
    else {
        m_displayLBalance = 0;
        m_displayRBalance = 0;
    }

    // --- D. 统计数据更新 ---
    if (instPower > m_maxPower) m_maxPower = instPower;
    m_sumPower += instPower;
    m_powerSampleCount++;

    if (m_displayCadence > 0) {
        m_sumCadence += m_displayCadence;
        m_cadenceSampleCount++;
    }
}

// ---------------------------------------------------------
// 2. 标准蓝牙功率计解析 (CPS)
// ---------------------------------------------------------
void XdsMonitor::parseStdPowerData(const uint8_t* data, int len) {
    if (len < 4) return;

    uint16_t flags = getUnsignedValue(data, 0);
    int16_t power = getSignedValue(data, 2);
    int offset = 4;

    if (flags & 0x0001) {
        uint8_t balanceRaw = data[offset];
        m_displayRBalance = (int)(balanceRaw * 0.5);
        m_displayLBalance = 100 - m_displayRBalance;
        offset += 1;
    }
    else {
        m_displayLBalance = 0; m_displayRBalance = 0;
    }

    m_displayPower = power;

    if (power > m_maxPower) m_maxPower = power;
    m_sumPower += power; m_powerSampleCount++;
}

// ---------------------------------------------------------
// 3. 心率带解析 (HRS)
// ---------------------------------------------------------
void XdsMonitor::parseHeartRateData(const uint8_t* data, int len) {
    if (len < 2) return;
    uint8_t flags = data[0];
    uint16_t hrValue = 0;

    if (flags & 0x01) {
        if (len >= 3) hrValue = getUnsignedValue(data, 1);
    }
    else {
        hrValue = data[1];
    }

    m_displayHeartRate = hrValue;
}

// ---------------------------------------------------------
// 4. 踏频传感器解析 (CSCP)
// ---------------------------------------------------------
void XdsMonitor::parseCscData(const uint8_t* data, int len) {
    if (len < 1) return;
    uint8_t flags = data[0];
    int offset = 1;

    if (flags & 0x01) {
        offset += 6;
    }

    if (flags & 0x02) {
        if (len < offset + 4) return;
        uint16_t cumCrank = getUnsignedValue(data, offset);
        uint16_t lastCrankEvent = getUnsignedValue(data, offset + 2);

        uint16_t instCadence = 0;

        if (m_firstPacket) {
            m_lastCrankCount = cumCrank;
            m_lastCrankTime = lastCrankEvent;
            m_firstPacket = false;
        }
        else {
            uint16_t diffCount = (cumCrank >= m_lastCrankCount) ?
                (cumCrank - m_lastCrankCount) :
                (65535 - m_lastCrankCount + cumCrank + 1);

            uint16_t diffTimeProto = (lastCrankEvent >= m_lastCrankTime) ?
                (lastCrankEvent - m_lastCrankTime) :
                (65535 - m_lastCrankTime + lastCrankEvent + 1);

            if (diffCount > 0 && diffTimeProto > 0) {
                instCadence = (uint16_t)((diffCount * 61440) / diffTimeProto);
                m_lastCrankCount = cumCrank;
                m_lastCrankTime = lastCrankEvent;
            }
        }
        m_displayCadence = instCadence;
        if (instCadence > 0) { m_sumCadence += instCadence; m_cadenceSampleCount++; }
    }
}

// ---------------------------------------------------------
// 通用 UI 刷新
// ---------------------------------------------------------
void XdsMonitor::refreshDisplay() {
    std::lock_guard<std::mutex> lock(m_printMutex);
    std::cout << "\r\033[K"; // 清行

    long long elapsedMs = millis() - m_startTime;
    int mins = (int)(elapsedMs / 60000);
    int secs = (int)((elapsedMs % 60000) / 1000);


    std::cout << "["
        << std::right << std::setw(2) << std::setfill('0') << mins << ":"
        << std::setw(2) << std::setfill('0') << secs << "] "
        << std::setfill(' ');

    switch (m_currentType) {
    case DeviceType::XDS_POWER:
    case DeviceType::STD_POWER:
 
        std::cout << "PWR:" << std::setw(3) << m_displayPower << "W "
            << "CAD:" << std::setw(3) << m_displayCadence << " "
            << "L/R:" << m_displayLBalance << "/" << m_displayRBalance << "%";
        if (m_currentType == DeviceType::XDS_POWER) {
            std::cout << " Ang:" << m_displayAngle;
        }
        break;

    case DeviceType::HEART_RATE:
        std::cout << "❤️ HR: " << std::setw(3) << m_displayHeartRate << " bpm "
            << " " << "" << "";
        break;

    case DeviceType::CSC_SENSOR:

        std::cout << "CAD:" << std::setw(3) << m_displayCadence << " rpm";
        break;

    default:
        std::cout << "等待数据...";
    }

    std::cout << std::flush;
}

bool XdsMonitor::initAdapter() {
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) { std::cerr << "无蓝牙适配器" << std::endl; return false; }
    m_adapter = adapters[0];
    return true;
}

void XdsMonitor::startMonitoring() {
    std::cout << "\n[3/3] 监控开始 (Ctrl+C 停止)..." << std::endl;

    m_startTime = millis();

    // 初始化 V2.1 算法状态
    m_first_calc = true;
    m_prev_revs = 0;

    // 重置 CSCP 兼容变量
    m_firstPacket = true;

    // 清空旧数据
    m_maxPower = 0; m_sumPower = 0; m_powerSampleCount = 0;
    m_sumCadence = 0; m_cadenceSampleCount = 0;
    m_displayPower = 0; m_displayCadence = 0; m_displayHeartRate = 0;

    try {
        m_targetDevice.notify(m_targetServiceUUID, m_targetCharUUID, [this](SimpleBLE::ByteArray bytes) {
            this->onDataReceived(bytes);
            });
    }
    catch (...) { return; }

    while (s_running && m_targetDevice.is_connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    try { m_targetDevice.unsubscribe(m_targetServiceUUID, m_targetCharUUID); }
    catch (...) {}
    try { m_targetDevice.disconnect(); }
    catch (...) {}
}

void XdsMonitor::run() {
    if (!initAdapter()) return;
    while (s_running) {
        if (scanAndSelectDevice()) {
            if (connectDevice()) startMonitoring();
        }
        else {
            if (s_running) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}