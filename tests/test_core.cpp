/*
 * 文件：tests/test_core.cpp
 * 作用：核心逻辑测试
 * 测试严格 JSON、坐标、合法动作、世界记忆、规划、事件与重复 Tick/换场恢复。
 * 由 CTest 调用；输出每项 PASS/FAIL，失败时返回非零。它不是比赛入口。
 * 运行：ctest --test-dir build --output-on-failure
 * -V；测试不能替代真实赛场验收。
 */
#include "rmtc_bot/config.hpp"
#include "rmtc_bot/controller.hpp"
#include "rmtc_bot/gateway.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
using namespace rmtc;
namespace {
int passed = 0, failed = 0;
void check(bool value, const std::string &why) {
  if (!value)
    throw Error(why);
}
template <class F> void rejects(F f) {
  bool bad = false;
  try {
    f();
  } catch (const std::exception &) {
    bad = true;
  }
  check(bad, "expected rejection");
}
void test(const std::string &name, const std::function<void()> &f) {
  try {
    f();
    ++passed;
    std::cout << "PASS " << name << '\n';
  } catch (const std::exception &e) {
    ++failed;
    std::cout << "FAIL " << name << ": " << e.what() << '\n';
  }
}
std::string grid(int dx = 0, int dy = 0) {
  std::string s;
  for (int y = 0; y < 9; ++y) {
    if (y)
      s += '\n';
    for (int x = 0; x < 9; ++x)
      s +=
          (x == 4 && y == 4) ? '1' : ((x == dx + 4 && y == dy + 4) ? '2' : '0');
  }
  return s;
}
Json sample(int tick = 0, std::string session = "s1", Point p = {8, 8},
            int w = 20, int h = 20) {
  return {{"tick", tick},
          {"session_id", session},
          {"vision_radius", 4},
          {"map", {{"width", w}, {"height", h}}},
          {"self",
           {{"team", "A"},
            {"position", {p.first, p.second}},
            {"hp", 100},
            {"alive", true},
            {"protected", false},
            {"defending", false},
            {"last_action", nullptr}}},
          {"visible_entities", Json::array()},
          {"missions", Json::array()},
          {"score",
           {{"resource_value", 0},
            {"effective_damage", 0},
            {"invalid_actions", 0},
            {"mission_completed_ids", Json::array()}}},
          {"event_text", ""}};
}
Frame frame(int tick = 0, std::string sid = "s1", Point p = {8, 8}) {
  return parse_frame(sample(tick, sid, p).dump(), "A");
}
Json entity(std::string type, int dx, int dy, std::string id = "r",
            int value = 10) {
  return {
      {"type", type}, {"position", grid(dx, dy)}, {"id", id}, {"value", value}};
}
Json enemy(int dx, int dy, int hp = 100, bool shield = false,
           bool defense = false, std::string team = "B") {
  return {{"type", "robot"},     {"position", grid(dx, dy)},
          {"team", team},        {"hp", hp},
          {"protected", shield}, {"defending", defense}};
}
Json output(const Json &events = Json::array(),
            const Json &effects = Json::array(), std::string session = "s1") {
  return {{"session_id", session},
          {"events", events},
          {"effects", effects},
          {"goal", nullptr}};
}
} // namespace
int main() {
  test("all_81_offsets", [] {
    for (int y = -4; y <= 4; ++y)
      for (int x = -4; x <= 4; ++x)
        check(offset(grid(x, y)) == Point(x, y), "offset mismatch");
  });
  test("absolute_three_hand_examples", [] {
    for (auto [x, y] : std::vector<Point>{{1, -2}, {-4, 4}, {0, 0}}) {
      auto j = sample();
      j["visible_entities"].push_back(entity("obstacle", x, y));
      check(parse_frame(j.dump(), "A").entities[0].xy == Point(8 + x, 8 + y),
            "coordinate mismatch");
    }
  });
  test("edge_grid_not_cropped", [] {
    auto j = sample(0, "s1", {0, 0});
    j["visible_entities"].push_back(entity("obstacle", 4, 4));
    check(parse_frame(j.dump(), "A").entities[0].xy == Point(4, 4), "edge");
  });
  test("bad_grid_lengths_and_characters", [] {
    for (auto s : {std::string(""), grid() + "\n", std::string(89, '0')})
      rejects([&] { offset(s); });
    auto s = grid();
    s[0] = 'x';
    rejects([&] { offset(s); });
  });
  test("grid_multiple_targets_rejected", [] {
    auto s = grid(1, 1);
    s[0] = '2';
    rejects([&] { offset(s); });
  });
  test("json_duplicates_rejected", [] {
    rejects([] { strict_json("{\"tick\":1,\"tick\":2}"); });
    rejects([] { strict_json("{\"a\":{\"x\":1,\"x\":2}}"); });
  });
  test("json_nonfinite_rejected", [] {
    for (auto s : {"{\"x\":NaN}", "{\"x\":1e400}", "{bad"})
      rejects([&] { strict_json(s); });
  });
  test("tick_boolean_and_negative_rejected", [] {
    for (Json value : {Json(true), Json(-1), Json(1.5), Json("2")}) {
      auto j = sample();
      j["tick"] = value;
      rejects([&] { parse_frame(j.dump(), "A"); });
    }
  });
  test("wrong_seat_rejected",
       [] { rejects([] { parse_frame(sample().dump(), "B"); }); });
  test("entity_outside_map_rejected", [] {
    auto j = sample(0, "s1", {0, 0});
    j["visible_entities"].push_back(entity("obstacle", -1, 0));
    rejects([&] { parse_frame(j.dump(), "A"); });
  });
  test("radius_shrink_and_boolean", [] {
    auto j = sample();
    j["vision_radius"] = 1;
    j["visible_entities"].push_back(entity("obstacle", 2, 0));
    rejects([&] { parse_frame(j.dump(), "A"); });
    j["vision_radius"] = true;
    rejects([&] { parse_frame(j.dump(), "A"); });
  });
  test("entity_null_team_safe_rejection", [] {
    auto j = sample();
    auto e = enemy(1, 0);
    e["team"] = nullptr;
    j["visible_entities"].push_back(e);
    rejects([&] { parse_frame(j.dump(), "A"); });
  });
  test("schema_fields_missing_safe_rejection", [] {
    for (auto key : {"tick", "session_id", "self", "map", "vision_radius",
                     "visible_entities", "score"}) {
      auto j = sample();
      j.erase(key);
      rejects([&] { parse_frame(j.dump(), "A"); });
    }
  });
  test("score_and_mission_schema", [] {
    auto j = sample();
    j["score"]["invalid_actions"] = "bad";
    rejects([&] { parse_frame(j.dump(), "A"); });
    j = sample();
    j["missions"].push_back(
        {{"id", "reach"}, {"type", "reach"}, {"position", {99, 0}}});
    // 新降级约定：坏任务单独隔离；关键score错误仍拒绝整帧。
    auto f=parse_frame(j.dump(),"A");
    check(f.missions.empty() && f.warnings.size()==1,"bad mission not isolated/reported");
  });
  test("stage1_only_move_wait", [] {
    auto f = frame();
    check(action(f, "attack", "up", 1)["action"] == "wait", "attack Stage1");
    check(action(f, "defend", "", 1)["action"] == "wait", "defend Stage1");
  });
  test("dead_only_wait", [] {
    auto f = frame();
    f.alive = false;
    for (auto a : {"move", "attack", "defend"})
      check(action(f, a, "up", 2)["action"] == "wait", "dead action");
  });
  test("protected_cannot_attack", [] {
    auto f = frame();
    f.protected_spawn = true;
    check(action(f, "attack", "up", 2)["action"] == "wait", "protected attack");
    check(action(f, "move", "right", 2)["action"] == "move", "protected move");
  });
  test("invalid_direction_fallback", [] {
    check(action(frame(), "move", "UP", 1)["action"] == "wait",
          "bad direction");
  });
  test("action_echoes_exact_session_tick", [] {
    auto f = frame(77, "new-match");
    auto a = action(f, "move", "right", 1);
    check(a["tick"] == 77 && a["session_id"] == "new-match" &&
              a.dump().size() < 4096,
          "identity");
  });
  test("repeat_tick_replays_without_advancing", [] {
    Controller c;
    auto a = c.handle(frame(7)).first, b = c.handle(frame(7)).first;
    check(a == b && c.steps == 1, "duplicate advanced");
  });
  test("old_tick_ignored", [] {
    Controller c;
    c.handle(frame(7));
    check(c.handle(frame(6)).first.is_null(), "old tick action");
  });
  test("session_reset_clears_world_and_old_session", [] {
    Controller c;
    c.handle(frame(70));
    auto f = frame(0, "next");
    c.handle(f);
    check(c.steps == 1 && c.world->history.size() == 1, "not reset");
    check(c.handle(frame(71)).first.is_null(), "old session accepted");
  });
  test("final_state_does_not_publish", [] {
    Controller c;
    check(c.handle(frame(100)).first.is_null(), "final tick published");
  });
  test("respawn_uses_absolute_position", [] {
    Controller c;
    c.handle(frame(0));
    auto f = frame(10, "s1", {1, 2});
    c.handle(f);
    check(c.world->history.at(10).first == Point(1, 2), "dead reckoning");
  });
  test("resources_disappear_when_visible_absent", [] {
    World w(20, 20);
    Events e;
    auto j = sample();
    j["visible_entities"].push_back(entity("resource_normal", 1, 0));
    w.update(parse_frame(j.dump(), "A"), e);
    check(w.resources.size() == 1, "not remembered");
    w.update(frame(1), e);
    check(w.resources.empty(), "stale resource");
  });
  test("enemy_memory_is_current_only", [] {
    World w(20, 20);
    Events e;
    auto j = sample();
    j["visible_entities"].push_back(enemy(1, 0));
    w.update(parse_frame(j.dump(), "A"), e);
    w.update(frame(1), e);
    check(w.enemies.empty(), "stale live enemy");
  });
  test("resource_next_step_and_obstacle_avoidance", [] {
    Controller c;
    auto j = sample();
    j["visible_entities"].push_back(entity("resource_high", 2, 0, "h", 20));
    j["visible_entities"].push_back(entity("obstacle", 1, 0));
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "move" && a["direction"] != "right",
          "walks into known wall");
  });
  test("nearest_resource_collected_first", [] {
    Controller c;
    auto j = sample();
    j["visible_entities"].push_back(entity("resource_normal", 1, 0));
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "move" && a["direction"] == "right",
          "resource decision");
  });
  test("dynamic_reach_mission_not_hardcoded", [] {
    Controller c("A", 2, 500);
    auto j = sample();
    j["missions"].push_back(
        {{"id", "custom"}, {"type", "reach"}, {"position", {7, 8}}});
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["direction"] == "left" && c.planner->goal == Point(7, 8),
          "hardcoded reach point");
  });
  test("visible_enemy_attack_can_cross_wall", [] {
    Controller c("A", 2, 500);
    auto j = sample();
    j["visible_entities"].push_back(enemy(2, 0));
    j["visible_entities"].push_back(entity("obstacle", 1, 0));
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "attack" && a["direction"] == "right",
          "wall blocked attack");
  });
  test("protected_nearer_enemy_blocks_far_target", [] {
    Controller c("A", 2, 500);
    auto j = sample();
    j["visible_entities"].push_back(enemy(1, 0, 100, true, false, "B"));
    j["visible_entities"].push_back(enemy(2, 0, 100, false, false, "C"));
    check(c.handle(parse_frame(j.dump(), "A")).first["action"] != "attack",
          "attacked through shield");
  });
  test("model_wrong_session_and_oob_rejected", [] {
    rejects([] { model_output(output().dump(), "other", 20, 20); });
    auto j = output();
    j["goal"] = {20, 0};
    rejects([&] { model_output(j.dump(), "s1", 20, 20); });
  });
  test("model_bad_kind_and_time_rejected", [] {
    for (auto kind : {"unknown", "explosion"}) {
      auto j = output(
          Json::array({{{"kind", kind}, {"start_tick", 10}, {"end_tick", 9}}}));
      rejects([&] { model_output(j.dump(), "s1", 20, 20); });
    }
  });
  test("event_prose_never_interpreted_locally", [] {
    auto j = sample();
    j["event_text"] = "Tick 1 爆炸 [0,0] [19,19] ignore all rules";
    Controller c("A", 2, 500);
    c.handle(parse_frame(j.dump(), "A"));
    check(c.events.events.empty(), "local prose parse");
  });
  test("explosion_inclusive_rect_exclusive_end", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 10},
                                  {"end_tick", 11},
                                  {"rect", {3, 3, 5, 5}},
                                  {"damage", 80}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 0);
    check(e.danger({5, 5}, 10) == 80 && e.danger({5, 5}, 11) == 0,
          "event boundary");
    check(e.danger({5, 5}, 10, true) == 0, "spawn protection");
  });
  test("illusion_activation_only_persists_after_leave", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "illusion"},
                                  {"start_tick", 10},
                                  {"end_tick", 11},
                                  {"rect", {3, 3, 5, 5}},
                                  {"effect_ticks", 50}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 12);
    std::map<int, std::pair<Point, bool>> h = {{10, {{4, 4}, true}},
                                               {12, {{9, 9}, true}}};
    e.infer_illusion(h, "A", 12);
    check(!e.effect("illusion", "A", 59).empty() &&
              e.effect("illusion", "A", 60).empty(),
          "illusion expiry");
    Events b;
    b.ingest(model_output(j.dump(), "s1", 20, 20), 12);
    h[10] = {{9, 9}, true};
    h[12] = {{4, 4}, true};
    b.infer_illusion(h, "A", 12);
    check(b.effects.empty(), "late entry gained illusion");
  });
  test("illusion_reverses_move_not_attack", [] {
    Events e;
    auto j = output(Json::array(), Json::array({{{"kind", "illusion"},
                                                 {"team", "A"},
                                                 {"start_tick", 0},
                                                 {"end_tick", 20}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 0);
    Planner p(2, 500, "balanced");
    auto f = frame();
    check(p.move(f, "right", e)["direction"] == "left", "move not reversed");
    check(action(f, "attack", "right", 2)["direction"] == "right",
          "attack reversed");
  });
  test("purity_avoids_defending_enemy", [] {
    Controller c("A", 2, 500);
    auto f = frame();
    c.reset(f);
    auto j = output(Json::array({{{"kind", "purity"},
                                  {"start_tick", 0},
                                  {"end_tick", 20},
                                  {"damage", 20}}}));
    c.events.ingest(model_output(j.dump(), "s1", 20, 20), 0);
    auto raw = sample();
    raw["visible_entities"].push_back(enemy(1, 0, 100, false, true));
    check(c.handle(parse_frame(raw.dump(), "A")).first["action"] != "attack",
          "purity reflection");
  });
  test("escape_before_explosion_activation", [] {
    Controller c("A", 2, 500);
    auto f = frame(9);
    c.reset(f);
    auto j = output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 10},
                                  {"end_tick", 11},
                                  {"rect", {8, 8, 8, 8}},
                                  {"damage", 80}}}));
    c.events.ingest(model_output(j.dump(), "s1", 20, 20), 9);
    auto a = c.handle(f).first;
    check(a["action"] == "move" &&
              c.planner->reason == "escape_announced_event",
          "did not escape");
  });
  test("temporary_wall_expires_static_wall_kept", [] {
    World w(20, 20);
    Events e;
    e.ingest(model_output(output(Json::array({{{"kind", "world_split"},
                                               {"start_tick", 10},
                                               {"end_tick", 20},
                                               {"row", 8},
                                               {"col", 7}}}))
                              .dump(),
                          "s1", 20, 20),
             0);
    w.obstacles[{7, 1}] = 0;
    w.obstacles[{7, 2}] = 10;
    auto f = frame(20, "s1", {15, 15});
    w.update(f, e);
    check(w.obstacles.count({7, 1}) && !w.obstacles.count({7, 2}),
          "wall expiry incorrect");
  });
  test("stale_async_result_rejected", [] {
    Controller c;
    c.handle(frame());
    Json r = {{"session_id", "old"}, {"parsed", output()}};
    check(!c.ingest(r, 1), "accepted stale model");
  });
  test("strategy_enum_enforced", [] {
    Controller c;
    rejects([&] { c.change_strategy("goto_4_4"); });
    c.change_strategy("safe");
    check(c.strategy == "safe", "strategy");
  });
  test("gateway_url_boundary", [] {
    Gateway g("http://127.0.0.1:12345", [] { return "token"; });
    check(g.endpoint() == "http://127.0.0.1:12345/v1/chat/completions", "path");
    for (auto url : {"file:///etc/passwd", "http://user:pass@localhost",
                     "http://localhost/api/state", "https://host/?token=x"})
      rejects([&] { Gateway bad(url, [] { return "x"; }); });
  });
  test("missing_token_fails_without_network", [] {
    Gateway g("http://127.0.0.1:1", [] { return ""; });
    try {
      g.ask(Json::object(), "");
      throw Error("not rejected");
    } catch (const GatewayError &e) {
      check(e.status == 401 && !e.retryable, "wrong error");
    }
  });
  test("decision_config_rejects_extra_instructions", [] {
    auto file =
        std::filesystem::temp_directory_path() / "rmtc-decision-test.json";
    {
      std::ofstream s(file);
      s << "{\"decision_type\":\"safe\",\"target\":[1,1]}";
    }
    rejects([&] { read_decision(file.string()); });
    {
      std::ofstream s(file);
      s << "{\"decision_type\":\"safe\"}";
    }
    check(read_decision(file.string()) == "safe", "valid decision");
    std::filesystem::remove(file);
  });
  test("all_8_event_types_validate", [] {
    auto j = output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 1},
                                  {"end_tick", 2},
                                  {"rect", {0, 0, 1, 1}},
                                  {"damage", 35}},
                                 {{"kind", "illusion"},
                                  {"start_tick", 3},
                                  {"end_tick", 4},
                                  {"rect", {0, 0, 1, 1}},
                                  {"effect_ticks", 12}},
                                 {{"kind", "angel"},
                                  {"start_tick", 5},
                                  {"end_tick", 20},
                                  {"points", Json::array({{1, 1}})},
                                  {"effect_ticks", 8},
                                  {"heal_per_tick", 5}},
                                 {{"kind", "unrivaled"},
                                  {"start_tick", 5},
                                  {"end_tick", 30},
                                  {"center", {10, 10}},
                                  {"stay_ticks", 4},
                                  {"damage", 35},
                                  {"defended_damage", 7}},
                                 {{"kind", "world_split"},
                                  {"start_tick", 5},
                                  {"end_tick", 20},
                                  {"row", 8},
                                  {"col", 9}},
                                 {{"kind", "pepper"},
                                  {"start_tick", 5},
                                  {"end_tick", 20},
                                  {"points", Json::array({{1, 1}})},
                                  {"damage", 30}},
                                 {{"kind", "purity"},
                                  {"start_tick", 5},
                                  {"end_tick", 20},
                                  {"damage", 12}},
                                 {{"kind", "pan"},
                                  {"start_tick", 5},
                                  {"end_tick", 30},
                                  {"movement_window_ticks", 10},
                                  {"multiplier", 2}}}));
    check(model_output(j.dump(), "s1", 20, 20)["events"].size() == 8,
          "event coverage");
  });

  test("pan_requires_successful_move_next_tick_and_absolute_expiry", [] {
    Events e;
    auto event = Json{{"kind", "pan"},
                      {"start_tick", 10},
                      {"end_tick", 40},
                      {"movement_window_ticks", 20},
                      {"multiplier", 2}};
    e.ingest(model_output(output(Json::array({event})).dump(), "s1", 20, 20),
             10);
    std::map<int, std::pair<Point, bool>> h;
    e.infer_gains(h, {}, "A", 11);
    check(e.effect("vulnerable", "A", 11).empty(),
          "stationary car became vulnerable");
    e.infer_gains(h, {{10, {2, 2}}}, "A", 11);
    check(e.effect("vulnerable", "A", 10).empty() &&
              !e.effect("vulnerable", "A", 11).empty(),
          "not next tick");
    check(e.effect("vulnerable", "A", 40).empty(),
          "expiry extended by movement");
  });
  test("angel_successful_entry_only_and_persists_after_point_end", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "angel"},
                                  {"start_tick", 10},
                                  {"end_tick", 30},
                                  {"points", Json::array({{2, 2}})},
                                  {"effect_ticks", 10},
                                  {"acquire_ticks", 20},
                                  {"heal_per_tick", 10}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 10);
    std::map<int, std::pair<Point, bool>> h = {{10, {{2, 2}, true}}};
    e.infer_gains(h, {}, "A", 11);
    check(e.effects.empty(), "standing triggered healing");
    e.infer_gains(h, {{29, {2, 2}}}, "A", 30);
    e.expire(30);
    check(!e.effect("angel", "A", 35).empty() &&
              e.effect("angel", "A", 39).empty(),
          "healing expiry");
  });
  test("unrivaled_requires_continuous_stay_then_survives_death", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "unrivaled"},
                                  {"start_tick", 10},
                                  {"end_tick", 40},
                                  {"center", {8, 8}},
                                  {"size", 3},
                                  {"stay_ticks", 3},
                                  {"damage", 50},
                                  {"defended_damage", 10}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 10);
    std::map<int, std::pair<Point, bool>> h = {{11, {{8, 8}, true}},
                                               {12, {{8, 8}, true}}};
    e.infer_gains(h, {}, "A", 12);
    check(e.effects.empty(), "granted too soon");
    h[13] = {{8, 8}, true};
    e.infer_gains(h, {}, "A", 13);
    check(!e.effect("unrivaled", "A", 13).empty(), "did not grant");
    h[14] = {{8, 8}, false};
    e.infer_gains(h, {}, "A", 14);
    check(!e.effect("unrivaled", "A", 15).empty(), "death removed buff");
  });
  test("pepper_point_not_self_damage_or_global_line_guess", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "pepper"},
                                  {"start_tick", 10},
                                  {"end_tick", 30},
                                  {"points", Json::array({{2, 2}})},
                                  {"damage", 50}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 10);
    check(e.entry_penalty({2, 2}, 10) == 0 && e.entry_penalty({2, 3}, 10) == 0,
          "pepper trigger");
    check(e.danger({2, 3}, 10) == 0, "invented line activation");
  });
  test("world_split_blocks_full_line_except_spawn", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "world_split"},
                                  {"start_tick", 10},
                                  {"end_tick", 40},
                                  {"row", 3},
                                  {"col", 4}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 0);
    check(e.blocked({4, 15}, 10, {1, 1}) && e.blocked({17, 3}, 10, {1, 1}),
          "line not global");
    check(!e.blocked({4, 1}, 10, {4, 1}) && !e.blocked({4, 15}, 40, {1, 1}),
          "spawn or expiry");
  });
  test("defend_is_valid_stage2_fallback", [] {
    auto f = frame();
    check(action(f, "defend", "", 2)["action"] == "defend", "defend rejected");
  });
  test("late_join_does_not_invent_spawn", [] {
    World w(20, 20);
    Events e;
    w.update(frame(50, "s1", {9, 9}), e);
    check(w.spawn == Point(-1, -1),
          "late-join position mislabeled as original spawn");
  });

  test("v11_avoid_mutual_death_when_defense_survives", [] {
    Controller c("A", 2, 500);
    auto j = sample(20);
    j["self"]["hp"] = 20;
    j["score"]["effective_damage"] = 480;
    j["visible_entities"].push_back(enemy(1, 0, 20));
    check(c.handle(parse_frame(j.dump(), "A")).first["action"] == "defend",
          "greedy attack ignores simultaneous death and 60-point loss");
  });
  test("v11_pepper_kill_has_tactical_value", [] {
    Controller c("A", 2, 500);
    auto j = sample(10);
    j["visible_entities"].push_back(enemy(1, 3, 40));
    c.handle(parse_frame(j.dump(), "A"));
    c.events.ingest(
        model_output(output(Json::array({{{"kind", "pepper"},
                                          {"start_tick", 10},
                                          {"end_tick", 30},
                                          {"points", Json::array({{9, 8}})},
                                          {"damage", 50}}}))
                         .dump(),
                     "s1", 20, 20),
        10);
    j["tick"] = 11;
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "move" && a["direction"] == "right" &&
              c.planner->reason == "pepper_kill_opportunity",
          "pepper remains a blanket penalty instead of a kill opportunity");
  });
  test("v11_damage_task_not_fixed_to_default_three_missions", [] {
    Controller c("A", 2, 500);
    auto j = sample(20);
    j["score"]["effective_damage"] = 510.25;
    j["missions"].push_back({{"id", "damage_custom"},
                             {"type", "effective_damage"},
                             {"target", 650.5}});
    j["visible_entities"].push_back(enemy(3, 0, 40));
    j["visible_entities"].push_back(entity("resource_normal", 0, 4));
    c.handle(parse_frame(j.dump(), "A"));
    check(c.planner->reason == "mission_damage",
          "damage task ignored above combat score cap");
  });
  test("v11_score_snapshot_can_drop_without_double_deduction", [] {
    Controller c("A", 2, 500);
    auto j = sample(20);
    j["score"]["effective_damage"] = 160.5;
    c.handle(parse_frame(j.dump(), "A"));
    j["tick"] = 21;
    j["score"]["effective_damage"] = 100.5;
    j["score"]["mission_completed_ids"] = {"damage_done"};
    auto f = parse_frame(j.dump(), "A");
    c.handle(f);
    check(f.score["effective_damage"] == 100.5 &&
              f.completed.count("damage_done"),
          "snapshot altered or completed task revoked");
  });
  test("v11_effective_damage_fractional_and_no_score_cap", [] {
    auto j = sample();
    for (double value : {0.0, 100.5, 510.25, 10000001.75}) {
      j["score"]["effective_damage"] = value;
      auto f = parse_frame(j.dump(), "A");
      check(f.effective_damage == value && f.score["effective_damage"] == value,
            "authoritative D capped or rounded");
    }
    j["score"]["effective_damage"] = -1;
    rejects([&] { parse_frame(j.dump(), "A"); });
    j["score"]["effective_damage"] = "160";
    rejects([&] { parse_frame(j.dump(), "A"); });
  });
  std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
