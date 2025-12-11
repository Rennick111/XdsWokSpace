#include <iostream>
#include <csignal>
#include "XdsMonitor.h"

// 信号处理包装函数
void signalHandler(int signum) {
    XdsMonitor::stop();
}

int main() {
    // 1. 设置控制台与信号
    signal(SIGINT, signalHandler);
#ifdef _WIN32
    system("chcp 65001 > nul");
#endif

    std::cout << "=== XDS 功率计 PC 监视工具 ===" << std::endl;
    std::cout << "提示: 请确保功率计已唤醒(轻拍三下功率计)，且手机APP已完全关闭！" << std::endl;

    // 2. 创建并运行监控器
    XdsMonitor monitor;
    monitor.run();

    return 0;
}