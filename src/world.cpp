/*
 * 文件：src/world.cpp
 * 作用：本队可见世界记忆（实现文件）
 * 记录已见地图、资源、当前位置和成功移动历史；敌人状态以当前感知为准。
 * 输入本队 Frame 与动作回执；输出规划所需的有限记忆，不读取裁判隐藏状态。
 * 配套文件：include/rmtc_bot/world.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/world.hpp"
#include <algorithm>
namespace rmtc {
void World::update(const Frame &f, const Events &events) {
  if (updated_tick == f.tick)
    return;
  if (spawn.first < 0 && f.alive &&
      (f.tick == 0 ||
       f.raw.at("self").value("position_reason", std::string()) == "initial"))
    spawn = f.position;
  history[f.tick] = {f.position, f.alive};
  while (!history.empty() && history.begin()->first < f.tick - 600)
    history.erase(history.begin());
  if (!f.receipt.is_null() && f.receipt.value("action", "") == "move" &&
      f.receipt.value("moved", false)) {
    int moved_tick = f.receipt.at("tick").get<int>();
    if (moved_tick == f.tick - 1 &&
        f.raw.at("self").value("position_reason", std::string()) != "respawn")
      successful_moves[moved_tick] = f.position;
  }
  while (!successful_moves.empty() &&
         successful_moves.begin()->first < f.tick - 600)
    successful_moves.erase(successful_moves.begin());
  enemies.clear();
  for (const auto &e : f.entities)
    if (e.type == "robot")
      enemies.push_back(e);
  std::set<Point> confirmed_obstacles;
  if (f.alive) {
    std::set<Point> visible, blocked;
    std::set<std::string> live_resources;
    for (int x = std::max(0, f.position.first - f.radius);
         x < std::min(width, f.position.first + f.radius + 1); ++x)
      for (int y = std::max(0, f.position.second - f.radius);
           y < std::min(height, f.position.second + f.radius + 1); ++y)
        visible.insert({x, y});
    seen.insert(visible.begin(), visible.end());
    for (const auto &e : f.entities) {
      if (e.type == "obstacle")
        blocked.insert(e.xy);
      else if (e.type == "resource_normal" || e.type == "resource_high") {
        resources[e.id] = e;
        resource_last_seen[e.id] = f.tick;
        live_resources.insert(e.id);
      }
    }
    for (auto p : visible)
      if (!blocked.count(p))
        obstacles.erase(p);
    for (auto p : blocked)
      obstacles.emplace(p, f.tick);
    confirmed_obstacles = blocked;
    for (auto it = resources.begin(); it != resources.end();)
      if (visible.count(it->second.xy) && !live_resources.count(it->first)) {
        resource_last_seen.erase(it->first);
        it = resources.erase(it);
      } else
        ++it;
    ++visits[f.position];
  }
  if (previous && !f.receipt.is_null() &&
      f.receipt.value("action", "") == "move" &&
      !f.receipt.value("moved", false) && f.alive &&
      (!f.receipt.contains("error") || f.receipt["error"] == "E_BLOCKED" ||
       f.receipt["error"] == "E_OOB")) {
    const auto &a = previous->second;
    std::string dir =
        f.receipt.value("effective_direction", a.value("direction", ""));
    if (f.receipt["tick"] == a["tick"] && directions.count(dir)) {
      auto d = directions.at(dir);
      blocked_until[{previous->first.first + d.first,
                     previous->first.second + d.second}] = f.tick + 2;
    }
  }
  for (const auto &row : events.events) {
    const auto &e = row.second;
    if (e["kind"] == "world_split" && f.tick >= e["end_tick"].get<int>()) {
      for (auto it = obstacles.begin(); it != obstacles.end();)
        if ((it->first.first == e["col"] || it->first.second == e["row"]) &&
            it->second >= e["start_tick"].get<int>() &&
            !confirmed_obstacles.count(it->first))
          // 临时墙到期仅撤销未再确认的记忆；当前感知仍存在的墙属于
          // 有效障碍，不能因首次看见它的时间晚于事件开始而删除。
          it = obstacles.erase(it);
        else
          ++it;
    }
  }
  for (auto it = blocked_until.begin(); it != blocked_until.end();)
    if (it->second <= f.tick)
      it = blocked_until.erase(it);
    else
      ++it;
  updated_tick = f.tick;
}
} // namespace rmtc
