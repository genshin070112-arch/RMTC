/*
 * 文件：src/controller.cpp
 * 作用：比赛状态与动作协调（实现文件）
 * 处理 session 切换、过期帧、重复 Tick 的缓存重放，以及异步模型结果的时效检查。
 * 输入感知与后台结果；调用世界/事件/规划模块，向 node 返回本 Tick 的动作。
 * 配套文件：include/rmtc_bot/controller.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/controller.hpp"
namespace rmtc {
void Controller::reset(const Frame &f) {
  if (!session.empty())
    retired.insert(session);
  session = f.session;
  tick = -1;
  steps = 0;
  cached = Json();
  world = std::make_unique<World>(f.width, f.height);
  events = Events();
  planner = std::make_unique<Planner>(stage, max_ticks, strategy, resource_cap);
}
std::pair<Json, std::string> Controller::handle(const Frame &f) {
  if (retired.count(f.session))
    return {Json(), "retired_session"};
  if (f.session != session)
    reset(f);
  if (f.width != world->width || f.height != world->height)
    return {Json(), "map_changed"};
  if (f.tick < tick)
    return {Json(), "old_tick"};
  if (f.tick == tick)
    return {cached, "replay"};
  tick = f.tick;
  cached = Json(); // Never replay the previous tick if planning throws.
  ++steps;
  world->update(f, events);
  events.infer_illusion(world->history, seat, f.tick);
  events.infer_gains(world->history, world->successful_moves, seat, f.tick);
  events.expire(f.tick);
  cached = planner->decide(f, *world, events);
  if (!cached.is_null())
    world->record(f, cached);
  return {cached, "new_tick"};
}
bool Controller::ingest(const Json &r, int now) {
  now = std::max(now, tick); // 乱序旧帧不能延长异步目标有效期。
  if (r.value("session_id", "") != session || !r.contains("parsed") ||
      r["parsed"].is_null() || r.value("provisional", false))
    return false;
  Json parsed = r["parsed"];
  if (r.value("source_tick", now) < now - 12)
    parsed["goal"] = nullptr;
  events.ingest(parsed, now);
  return true;
}
void Controller::change_strategy(const std::string &value) {
  if (!strategies.count(value))
    throw Error("unregistered strategy");
  strategy = value;
  if (planner)
    planner->strategy = value;
}
} // namespace rmtc
