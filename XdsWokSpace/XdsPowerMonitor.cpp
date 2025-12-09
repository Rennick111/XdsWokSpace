#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>
#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <algorithm>
#include <string>
#include <cctype>

// 引入 SimpleBLE 库
#include <simpleble/SimpleBLE.h>

// --- 配置区域 ---
// 目标服务 UUID (XDS 功率计通常使用 0x1828
const std::string TARGET_UUID_SUBSTRING = "1828";
const std::string CHAR_UUID_SUBSTRING = "2a63"; // 功率数据特征值

// --- 全局控制变量 ---
std::atomic<bool> g_running{ true };
std::atomic<long long> g_lastDataTime{ 0 };
std::mutex g_printMutex;

// --- 辅助工具函数 ---

// 获取当前毫秒时间戳
long long millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// 信号处理 (Ctrl+C)
void signalHandler(int signum) {
    g_running = false;
}

// --- 数据解析 (完全复刻原版固件) ---
uint16_t getUnsignedValue(const uint8_t* data, int offset) {
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

int16_t getSignedValue(const uint8_t* data, int offset) {
    uint16_t val = getUnsignedValue(data, offset);
    return (int16_t)val;
}

// --- 数据回调处理 ---
void onDataReceived(SimpleBLE::ByteArray bytes) {
    g_lastDataTime = millis();
    const uint8_t* data = (const uint8_t*)bytes.c_str();

    // 安全检查：包长度必须足够
    if (bytes.length() < 11) return;

    // 1. 解析核心数据
    uint16_t totalPower = getUnsignedValue(data, 0);
    int16_t leftPower = getSignedValue(data, 2);
    int16_t rightPower = getSignedValue(data, 4);
    uint16_t cadence = getUnsignedValue(data, 8);
    uint8_t errorCode = data[10];

    // 2. 异常值过滤
    if (totalPower > 2000) return;

    // 3. 格式化输出 (使用 ANSI 清屏码 \033[K 避免残影)
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << "\r\033[K"
        << "⚡ 功率: " << std::left << std::setw(4) << totalPower << " W | "
        << "🔄 踏频: " << std::setw(4) << cadence << " RPM | "
        << "⚖️  左/右: " << std::setw(4) << leftPower << " / " << std::setw(4) << rightPower << " | "
        << "Err: " << (int)errorCode
        << std::flush;
}

int main() {
    // 1. 系统初始化
    signal(SIGINT, signalHandler);
#ifdef _WIN32
    system("chcp 65001 > nul"); // 修复 Windows 控制台中文乱码
#endif

    std::cout << "=== XDS 功率计 PC 诊断工具 (v3.0) ===" << std::endl;
    std::cout << "提示: 请确保功率计已唤醒(转动曲柄)，且手机APP已完全关闭！" << std::endl;

    // 2. 获取蓝牙适配器
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "[错误] 未检测到蓝牙适配器！请检查电脑蓝牙是否开启。" << std::endl;
        return 1;
    }
    auto adapter = adapters[0];
    std::cout << "[系统] 使用适配器: " << adapter.identifier() << " [" << adapter.address() << "]" << std::endl;

    SimpleBLE::Peripheral targetDevice;
    bool isTargetSelected = false;

    // --- 阶段 A: 交互式扫描与选择 ---
    while (g_running && !isTargetSelected) {
        std::cout << "\n[1/3] 正在扫描周围设备 (5秒)..." << std::endl;

        adapter.scan_for(5000);
        std::vector<SimpleBLE::Peripheral> peripherals = adapter.scan_get_results();

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
            std::string addr = p.address();
            int rssi = p.rssi();

            // 检查 UUID
            std::string uuidListStr = "";
            bool isCandidate = false;
            for (auto& service : p.services()) {
                std::string uuid = service.uuid();
                // 简化显示：只显示前8位
                uuidListStr += uuid.substr(0, 8) + ".. ";

                // 智能推荐算法：如果包含 1828
                if (uuid.find(TARGET_UUID_SUBSTRING) != std::string::npos) {
                    isCandidate = true;
                }
            }

            // 智能推荐算法：如果名字包含 XDS
            if (name.find("XDS") != std::string::npos || name.find("Power") != std::string::npos) {
                isCandidate = true;
            }

            std::cout << (isCandidate ? "⭐" : "  ")
                << "[" << index << "] "
                << std::left << std::setw(20) << (name.empty() ? "<无名称>" : name) << " | "
                << addr << " | "
                << std::setw(4) << rssi << " | "
                << uuidListStr
                << std::endl;

            if (isCandidate && recommendedIndex == -1) {
                recommendedIndex = index;
            }
            index++;
        }
        std::cout << "---------------------------------------------------------------" << std::endl;

        // 用户交互选择
        std::cout << "请输入要连接的设备 ID (输入 r 重新扫描): ";
        if (recommendedIndex != -1) {
            std::cout << "[推荐: " << recommendedIndex << "] ";
        }

        std::string input;
        std::cin >> input;

        if (input == "r" || input == "R") {
            continue;
        }

        try {
            int selectedId = std::stoi(input);
            if (selectedId >= 0 && selectedId < peripherals.size()) {
                targetDevice = peripherals[selectedId];
                isTargetSelected = true;
            }
            else {
                std::cout << "[错误] 无效的 ID，请重新输入。" << std::endl;
            }
        }
        catch (...) {
            std::cout << "[错误] 输入无效。" << std::endl;
        }
    }

    if (!g_running) return 0;

    // --- 阶段 B: 连接与服务发现 ---
    std::cout << "\n[2/3] 正在连接 [" << targetDevice.identifier() << "]..." << std::endl;

    try {
        targetDevice.connect();
        std::cout << "[系统] 连接成功！" << std::endl;
    }
    catch (std::exception& e) {
        std::cerr << "[错误] 连接失败: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[系统] 正在寻找 XDS 服务..." << std::endl;
    // 稍微延时等待服务表就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::string foundServiceUUID = "";
    std::string foundCharUUID = "";

    // 动态寻找匹配的 UUID (因为不同设备可能有细微差别)
    for (auto& service : targetDevice.services()) {
        if (service.uuid().find(TARGET_UUID_SUBSTRING) != std::string::npos) {
            foundServiceUUID = service.uuid();
            // 在该服务下找特征值
            for (auto& characteristic : service.characteristics()) {
                if (characteristic.uuid().find(CHAR_UUID_SUBSTRING) != std::string::npos) {
                    foundCharUUID = characteristic.uuid();
                    break;
                }
            }
        }
        if (!foundServiceUUID.empty() && !foundCharUUID.empty()) break;
    }

    if (foundServiceUUID.empty() || foundCharUUID.empty()) {
        std::cerr << "[错误] 未能在该设备上找到功率计服务 (0x1828) 或特征值 (0x2A63)！" << std::endl;
        std::cerr << "       这可能不是正确的 XDS 功率计，或者固件版本不兼容。" << std::endl;
        targetDevice.disconnect();
        return 1;
    }

    std::cout << "[系统] 锁定服务 UUID: " << foundServiceUUID << std::endl;
    std::cout << "[系统] 锁定特征 UUID: " << foundCharUUID << std::endl;

    // --- 阶段 C: 监控 ---
    std::cout << "\n[3/3] 开始监控数据 (按 Ctrl+C 退出)..." << std::endl;

    try {
        targetDevice.notify(foundServiceUUID, foundCharUUID, onDataReceived);
        g_lastDataTime = millis();
    }
    catch (std::exception& e) {
        std::cerr << "[错误] 订阅通知失败: " << e.what() << std::endl;
        targetDevice.disconnect();
        return 1;
    }

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!targetDevice.is_connected()) {
            std::cout << "\n[错误] 连接意外断开！程序即将退出。" << std::endl;
            break;
        }

        // 超时检测
        if (millis() - g_lastDataTime > 5000) {
            std::lock_guard<std::mutex> lock(g_printMutex);
            std::cout << "\r\033[K" << "[状态] 无数据 (可能已休眠)..." << std::flush;
        }
    }

    // 清理
    std::cout << "\n[系统] 断开连接..." << std::endl;
    try {
        if (targetDevice.is_connected()) {
            targetDevice.unsubscribe(foundServiceUUID, foundCharUUID);
            targetDevice.disconnect();
        }
    }
    catch (...) {}

    return 0;
}