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

const std::string UUID_CHR_XDS_DATA = "2a63"; // XDS 数据 (复用了 CPS UUID 但格式不同)
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
                    detectedType = DeviceType::CSC_SENSOR; typeStr = "踏频/速度"; break;
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
            if (sel >= 0 && sel < peripherals.size()) {
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
        std::cout << ">> 识别模式: 踏频/速度传感器 (CSCP)" << std::endl;
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
    const uint8_t* data = (const uint8_t*)bytes.c_str();
    int len = bytes.length();

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
// 1. XDS 原始解析逻辑 (保持不变)
// ---------------------------------------------------------
void XdsMonitor::parseXdsData(const uint8_t* data, int len) {
    if (len < 10) return;

    // 解析基础值
    uint16_t instPower = getUnsignedValue(data, 0);
    int16_t leftPower = getSignedValue(data, 2);
    int16_t rightPower = getSignedValue(data, 4);
    int16_t angle = (len >= 8) ? getSignedValue(data, 6) : 0;
    uint16_t rawCadence = getUnsignedValue(data, 8);

    if (instPower > 3000) instPower = 0; // 简单过滤

    // 踏频算法
    uint16_t instCadence = 0;
    long long now = millis();
    if (m_firstPacket) {
        m_lastCrankCount = rawCadence;
        m_lastCrankTime = now;
        m_firstPacket = false;
    }
    else {
        uint16_t diff = (rawCadence >= m_lastCrankCount) ?
            (rawCadence - m_lastCrankCount) :
            (65535 - m_lastCrankCount + rawCadence + 1);
        long long dt = now - m_lastCrankTime;

        if (diff > 0 && dt > 0) {
            instCadence = (uint16_t)((diff * 60000) / dt);
            if (instCadence > 200) instCadence = 200; // 异常过滤
            m_lastCrankCount = rawCadence;
            m_lastCrankTime = now;
        }
        else if (dt > 2500) {
            instCadence = 0; // 超时归零
        }
    }

    // 更新显示缓存
    m_displayPower = instPower;
    m_displayCadence = instCadence;
    m_displayAngle = angle;

    // 左右平衡
    int totalLR = std::abs(leftPower) + std::abs(rightPower);
    if (totalLR > 0) {
        m_displayLBalance = (std::abs(leftPower) * 100) / totalLR;
        m_displayRBalance = 100 - m_displayLBalance;
    }

    // 统计
    if (instPower > m_maxPower) m_maxPower = instPower;
    m_sumPower += instPower; m_powerSampleCount++;
    if (instCadence > 0) { m_sumCadence += instCadence; m_cadenceSampleCount++; }
}

// ---------------------------------------------------------
// 2. 标准蓝牙功率计解析 (CPS)
// ---------------------------------------------------------
void XdsMonitor::parseStdPowerData(const uint8_t* data, int len) {
    // 格式: Flags(2B) + Power(2B) + [Balance(1B)] + [Crank(2B+2B)] ...
    if (len < 4) return;

    uint16_t flags = getUnsignedValue(data, 0);
    int16_t power = getSignedValue(data, 2);
    int offset = 4;

    // Flag Bit 0: Pedal Power Balance Present
    if (flags & 0x0001) {
        uint8_t balanceRaw = data[offset];
        // 标准定义: 1/2% unit. 暂简单处理为百分比
        m_displayRBalance = (int)(balanceRaw * 0.5); // 通常值是右腿百分比 * 2
        m_displayLBalance = 100 - m_displayRBalance;
        offset += 1;
    }
    else {
        m_displayLBalance = 0; m_displayRBalance = 0;
    }

    // Flag Bit 1: Accumulated Torque (Not used here) -> offset += 2
    // Flag Bit 2: Wheel Revolution (Not used here) -> offset += 6

    // Flag Bit 5: Crank Revolution Data Present (用于算踏频)
    // 注意: 要准确跳过前面的字段需要完整解析 Flags，这里做个简化假设:
    // 如果没有 PedalBalance，通常 Power 后紧接的就是扩展数据，但这不严谨。
    // 很多单边功率计不发 Balance，但发 Crank Data。
    // 简单的标准顺序检测: 
    // Offset calc: 2(Flags) + 2(Power)
    // if bit0: +1
    // if bit1: +2
    // if bit2: +6 (4 Cumulative Wheel + 2 Last Wheel Time)
    // if bit3: +4 (Extreme Magnitudes) ...
    // 为简化代码，此处只解析功率。若需踏频，通常需更严谨的 Flags 解析。

    m_displayPower = power;

    // 更新统计
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

    // Bit 0: 0=uint8, 1=uint16
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

    // Bit 0: Wheel Rev Data Present
    if (flags & 0x01) {
        offset += 6; // uint32 cumWheel + uint16 lastWheelTime
    }

    // Bit 1: Crank Rev Data Present
    if (flags & 0x02) {
        if (len < offset + 4) return;
        uint16_t cumCrank = getUnsignedValue(data, offset);
        uint16_t lastCrankEvent = getUnsignedValue(data, offset + 2); // unit: 1/1024s

        // 踏频算法 (CSCP 的时间单位是 1/1024 秒)
        long long now = millis();
        uint16_t instCadence = 0;

        if (m_firstPacket) {
            m_lastCrankCount = cumCrank;
            m_lastCrankTime = lastCrankEvent; // 这里存的是协议时间
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
                // RPM = (Count * 1024 * 60) / Time_units
                // 简化: (Count * 61440) / Time
                instCadence = (uint16_t)((diffCount * 61440) / diffTimeProto);

                m_lastCrankCount = cumCrank;
                m_lastCrankTime = lastCrankEvent;
            }

            // 超时检测需要用系统时间辅助，这里略过复杂实现，仅做简单计算
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
    int mins = elapsedMs / 60000;
    int secs = (elapsedMs % 60000) / 1000;
    int avgPower = (m_powerSampleCount > 0) ? (m_sumPower / m_powerSampleCount) : 0;
    int avgCad = (m_cadenceSampleCount > 0) ? (m_sumCadence / m_cadenceSampleCount) : 0;

    std::cout << "[" << std::setw(2) << std::setfill('0') << mins << ":"
        << std::setw(2) << std::setfill('0') << secs << "] "
        << std::setfill(' ');

    // 根据设备类型显示不同内容
    switch (m_currentType) {
    case DeviceType::XDS_POWER:
    case DeviceType::STD_POWER:
        std::cout << "PWR:" << std::setw(3) << m_displayPower << "/"
            << std::setw(3) << avgPower << "W "
            << "CAD:" << std::setw(3) << m_displayCadence << " "
            << "L/R:" << m_displayLBalance << "/" << m_displayRBalance << "%";
        if (m_currentType == DeviceType::XDS_POWER) {
            std::cout << " Ang:" << m_displayAngle;
        }
        break;

    case DeviceType::HEART_RATE:
        std::cout << "❤️ HR: " << std::setw(3) << m_displayHeartRate << " bpm "
            << "(MAX: " << "-" << ")"; // 简单起见不统计最大心率
        break;

    case DeviceType::CSC_SENSOR:
        std::cout << "CAD:" << std::setw(3) << m_displayCadence << "/"
            << std::setw(3) << avgCad << " rpm";
        break;

    default:
        std::cout << "等待数据...";
    }

    std::cout << std::flush;
}

// ... initAdapter, run 等函数保持结构不变，调用 scanAndSelectDevice 即可 ...
// 为了节省篇幅，initAdapter, startMonitoring (除 onDataReceived 绑定外), run 等逻辑
// 基本可以复用原有结构，只需确保 startMonitoring 中调用 refreshDisplay() 即可。
// 务必记得在 startMonitoring 里把 m_firstPacket = true 重置。

bool XdsMonitor::initAdapter() {
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) { std::cerr << "无蓝牙适配器" << std::endl; return false; }
    m_adapter = adapters[0];
    return true;
}

void XdsMonitor::startMonitoring() {
    std::cout << "\n[3/3] 监控开始 (Ctrl+C 停止)..." << std::endl;

    m_startTime = millis();
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