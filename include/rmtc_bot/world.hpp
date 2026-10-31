/*
 * 文件：include/rmtc_bot/world.hpp
 * 作用：本队可见世界记忆（接口声明；具体行为见对应 .cpp）
 * 记录已见地图、资源、当前位置和成功移动历史；敌人状态以当前感知为准。
 * 输入本队 Frame 与动作回执；输出规划所需的有限记忆，不读取裁判隐藏状态。
 * 配套文件：src/world.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include "events.hpp"
#include <optional>
namespace rmtc {
class World {
public:
  int width, height, updated_tick = -1;
  Point spawn = {-1, -1};
  std::set<Point> seen;
  std::map<Point, int> obstacles, visits, blocked_until;
  std::map<std::string, Entity> resources;
  std::map<std::string, int>
      resource_last_seen; // Stage2未知区域的记忆可信度随时间下降。
  std::vector<Entity> enemies;
  std::map<int, std::pair<Point, bool>> history;
  std::map<int, Point> successful_moves;
  std::optional<std::pair<Point, Json>> previous;
  World(int w, int h) : width(w), height(h) {}
  void update(const Frame &, const Events &);
  void record(const Frame &f, const Json &a) {
    previous = std::make_pair(f.position, a);
  }
};
} // namespace rmtc
