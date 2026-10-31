/*
 * 文件：tests/test_review_rules.cpp
 * 作用：规则二次审查回归，覆盖世界分裂墙体到期与迟到天使公告的恢复。
 * 由 CTest 或独立编译运行；使用手算时间线，不调用模型或读取裁判隐藏状态。
 */
#include "rmtc_bot/world.hpp"
#include <functional>
#include <iostream>
using namespace rmtc;
namespace {
int passed = 0, failed = 0;
void check(bool ok, const std::string &why) {
  if (!ok)
    throw Error(why);
}
void test(const std::string &name, const std::function<void()> &run) {
  try {
    run();
    ++passed;
    std::cout << "PASS " << name << '\n';
  } catch (const std::exception &e) {
    ++failed;
    std::cout << "FAIL " << name << ": " << e.what() << '\n';
  }
}
Frame frame(int tick, Point p) {
  Frame f;
  f.tick = tick;
  f.width = f.height = 20;
  f.radius = 4;
  f.seat = "A";
  f.session = "s";
  f.position = p;
  f.alive = true;
  f.hp = 100;
  f.raw = {{"self", Json::object()}};
  return f;
}
Json output(Json event) {
  return {{"events", Json::array({event})},
          {"effects", Json::array()},
          {"goal", nullptr}};
}
Json split() {
  return {{"kind", "world_split"},
          {"start_tick", 10},
          {"end_tick", 20},
          {"row", 8},
          {"col", 7}};
}
Json angel(int acquire = 20) {
  return {{"kind", "angel"},    {"start_tick", 10},
          {"end_tick", 30},     {"points", Json::array({{2, 2}})},
          {"effect_ticks", 10}, {"acquire_ticks", acquire},
          {"heal_per_tick", 10}};
}
} // namespace
int main() {
  test("split_expiry_keeps_currently_visible_original_wall", [] {
    Events e;
    e.ingest(output(split()), 0);
    World w(20, 20);
    auto f = frame(20, {8, 8});
    Entity wall;
    wall.type = "obstacle";
    wall.xy = {7, 8};
    f.entities = {wall};
    w.update(f, e);
    check(w.obstacles.count({7, 8}), "discarded current authoritative wall");
  });
  test("split_expiry_removes_unseen_temporary_wall_keeps_known_static", [] {
    Events e;
    e.ingest(output(split()), 0);
    World w(20, 20);
    w.obstacles[{7, 1}] = 0;
    w.obstacles[{7, 2}] = 10;
    w.update(frame(20, {15, 15}), e);
    check(w.obstacles.count({7, 1}), "discarded known static wall");
    check(!w.obstacles.count({7, 2}), "retained obsolete temporary wall");
  });
  test("angel_late_notice_restores_still_active_gain", [] {
    Events e;
    e.ingest(output(angel()), 31);
    e.infer_gains({}, {{29, {2, 2}}}, "A", 31);
    e.expire(31);
    check(e.active("angel", 31).empty(), "expired acquisition point revived");
    check(!e.effect("angel", "A", 31).empty(), "lost still-active late gain");
    check(!e.effect("angel", "A", 38).empty(), "healing ended too early");
    check(e.effect("angel", "A", 39).empty(), "healing exceeded exact expiry");
  });
  test("angel_late_notice_retention_uses_acquisition_window", [] {
    Events e;
    e.ingest(output(angel(5)), 31);
    e.infer_gains({}, {{14, {2, 2}}, {29, {2, 2}}}, "A", 31);
    e.expire(31);
    check(e.events.empty(), "retained a wholly expired event");
    check(e.effect("angel", "A", 31).empty(), "invented late acquisition");
  });
  test("angel_notice_at_effect_expiry_does_not_revive_healing", [] {
    Events e;
    e.ingest(output(angel()), 39);
    e.infer_gains({}, {{29, {2, 2}}}, "A", 39);
    e.expire(39);
    check(e.events.empty(), "kept fully expired inference data");
    check(e.effect("angel", "A", 39).empty(), "revived expired effect");
  });
  std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
