/*
 * 文件：include/rmtc_bot/events.hpp
 * 作用：事件与效果管理（接口声明；具体行为见对应 .cpp）
 * 校验模型输出的结构化事件、效果和目标，管理有效期，并根据合法历史推断部分效果。
 * 输入模型 JSON、感知与本队历史；输出危险区域、效果状态等供 controller/planner
 * 使用。
 * 配套文件：src/events.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include "protocol.hpp"
namespace rmtc {
Json model_output(std::string content, const std::string &session, int w,
                  int h);
bool inside(Point, const Json &rect);
class Events {
public:
  std::map<std::pair<std::string, int>, Json> events;
  std::map<std::tuple<std::string, std::string, int>, Json> effects;
  Point goal = {-1, -1};
  int goal_until = -1;
  void ingest(const Json &, int now);
  void expire(int tick);
  std::vector<Json> active(const std::string &, int tick) const;
  std::vector<Json> effect(const std::string &, const std::string &team,
                           int tick) const;
  void infer_illusion(const std::map<int, std::pair<Point, bool>> &,
                      const std::string &, int);
  void infer_gains(const std::map<int, std::pair<Point, bool>> &history,
                   const std::map<int, Point> &successful_moves,
                   const std::string &team, int tick);
  bool blocked(Point, int tick, Point spawn) const;
  double danger(Point, int tick, bool protected_spawn = false) const;
  double entry_penalty(Point, int tick) const;
};
} // namespace rmtc
