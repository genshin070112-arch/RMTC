/*
 * 文件：include/rmtc_bot/config.hpp
 * 作用：启动配置与令牌读取（接口声明；具体行为见对应 .cpp）
 * 读取命令行、白名单环境配置、令牌文件和 decision_config.json；检查配置合法性。
 * 输入启动参数/本地配置；输出 Config。由 node 使用；.env 作为数据读取，不当作
 * shell 执行。
 * 配套文件：src/config.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include "protocol.hpp"
namespace rmtc {
struct Config {
  std::string seat = "A", name = "RMTC-CPP", gateway = "http://127.0.0.1:8080",
              model, token_file;
  std::string decision_file = "config/decision_config.json", log_dir = "logs",
              strategy = "balanced";
  int stage = 1, max_ticks = 0, timeout_ms = 4000, budget = 48,
      max_tokens = 1600;
  double resource_cap = 120;
};
Config config(int argc, char **argv);
std::string read_token(const std::string &path);
std::string read_decision(const std::string &path);
std::string read_file(const std::string &, size_t limit);
} // namespace rmtc
