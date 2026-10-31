/*
 * 文件：include/rmtc_bot/controller.hpp
 * 作用：比赛状态与动作协调（接口声明；具体行为见对应 .cpp）
 * 处理 session 切换、过期帧、重复 Tick 的缓存重放，以及异步模型结果的时效检查。
 * 输入感知与后台结果；调用世界/事件/规划模块，向 node 返回本 Tick 的动作。
 * 配套文件：src/controller.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include "planner.hpp"
#include <memory>
namespace rmtc {
class Controller {
public:
  std::string seat, session, strategy;
  int stage, max_ticks, tick = -1, steps = 0;
  double resource_cap;
  std::set<std::string> retired;
  Json cached;
  std::unique_ptr<World> world;
  Events events;
  std::unique_ptr<Planner> planner;
  Controller(std::string s = "A", int st = 1, int n = 100,
             std::string mode = "balanced", double cap = 120)
      : seat(std::move(s)), strategy(std::move(mode)), stage(st), max_ticks(n),
        resource_cap(cap) {}
  void reset(const Frame &);
  std::pair<Json, std::string> handle(const Frame &);
  bool ingest(const Json &result, int now);
  void change_strategy(const std::string &);
};
} // namespace rmtc
