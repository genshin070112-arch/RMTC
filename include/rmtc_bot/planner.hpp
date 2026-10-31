/*
 * 文件：include/rmtc_bot/planner.hpp
 * 作用：动作规划与策略（接口声明；具体行为见对应 .cpp）
 * 用加权最短路、资源/任务收益、战斗和危险代价，选择本 Tick 的动作。
 * 输入 Frame、World、Events 与策略枚举；输出动作。由 controller
 * 调用，不等待模型请求。
 * 配套文件：src/planner.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include "world.hpp"
namespace rmtc {
extern const std::set<std::string> strategies;
struct Search {
  std::map<Point, double> cost, damage;
  std::map<Point, int> depth; // 真实步数与加权偏好分离，不用 cost 推算Tick。
  std::map<Point, std::string> first;
};
class Planner {
  // 追逐（包括中间巡逻）而有效伤害未增长时短暂改做其他目标，避免动态碰撞循环。
  int pursuit_stalled_ = 0, pursuit_until_ = -1, last_decision_tick_ = -1;
  double last_damage_ = 0;
public:
  int stage, max_ticks;
  double resource_cap;
  std::string strategy, reason = "initial";
  Point goal = {-1, -1};
  Planner(int s, int n, std::string mode, double cap = 120)
      : stage(s), max_ticks(n), resource_cap(cap), strategy(std::move(mode)) {}
  Search search(const Frame &, const World &, const Events &) const;
  Json decide(const Frame &, const World &, const Events &);
  Json move(const Frame &, std::string, const Events &) const;
};
} // namespace rmtc
