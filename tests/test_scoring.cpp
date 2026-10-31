/*
 * 文件：tests/test_scoring.cpp
 * 作用：v1.1 的计分公式、分奖批次、死亡顺序及战术预测测试。
 * 输入手算的血量/命中与公开感知夹具；比较比例贡献、60点去重分奖、500门槛和任务保留。
 * 由 CTest 运行；这是模型/单元验证，另须用新版裁判作独立对照，不代表真实 LLM
 * 验收。
 */
#include "rmtc_bot/tactics.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
using namespace rmtc;
namespace {
int passed = 0, failed = 0;
void check(bool yes, const std::string &why) {
  if (!yes)
    throw Error(why);
}
void near(double a, double b) {
  check(std::abs(a - b) < 1e-8,
        "expected " + std::to_string(b) + ", got " + std::to_string(a));
}
void test(const std::string &name, std::function<void()> run) {
  try {
    run();
    ++passed;
    std::cout << "PASS " << name << '\n';
  } catch (const std::exception &e) {
    ++failed;
    std::cout << "FAIL " << name << ": " << e.what() << '\n';
  }
}
Frame base() {
  Frame f;
  f.seat = "A";
  f.session = "s1";
  f.position = {5, 5};
  f.tick = 10;
  f.width = f.height = 20;
  f.radius = 4;
  f.hp = 100;
  f.alive = true;
  f.missions = Json::array();
  f.score = {{"effective_damage", 0}};
  return f;
}
Entity car(std::string team, Point p, double hp = 100, bool defending = false,
           bool protected_spawn = false) {
  Entity e;
  e.type = "robot";
  e.team = team;
  e.xy = p;
  e.hp = hp;
  e.defending = defending;
  e.protected_spawn = protected_spawn;
  return e;
}
Events event(const std::string &kind, Json params = Json::object()) {
  params["kind"] = kind;
  params["start_tick"] = 0;
  params["end_tick"] = 30;
  Events e;
  e.ingest({{"events", Json::array({params})},
            {"effects", Json::array()},
            {"goal", nullptr}},
           10);
  return e;
}
} // namespace
int main() {
  test("500_full_score_and_uncapped_tiebreak", [] {
    near(combat_score(300), 9);
    near(combat_score(500), 15);
    near(combat_score(860.25), 15);
    near(projected_damage(500, 20, 60, 0), 580);
    check(damage_utility(500, 580) > 0, "lost tiebreak benefit");
  });
  test("add_all_credits_before_death_floor", [] {
    near(projected_damage(80, 20, 60, 0), 160);
    near(projected_damage(160, 0, 0, 1), 100);
    near(projected_damage(0, 20, 60, 1), 20);
    near(projected_damage(10, 10, 0, 1), 0);
    near(projected_damage(160, 0, 0, 2), 40);
  });
  test("overkill_proportional_and_shared_reward", [] {
    auto b = resolve_batch({{"A", 100}, {"B", 10}, {"C", 100}},
                           {{"A", "B", 20, true}, {"C", "B", 20, true}});
    near(b.actual_attack["A"], 5);
    near(b.actual_attack["C"], 5);
    near(b.bonuses["A"], 30);
    near(b.bonuses["C"], 30);
    near(b.hp["B"], 0);
  });
  test("same_source_deduplicated_for_kill", [] {
    auto b = resolve_batch(
        {{"A", 100}, {"B", 10}, {"C", 100}},
        {{"A", "B", 20, true}, {"A", "B", 20, true}, {"C", "B", 20, true}});
    near(b.actual_attack["A"], 20.0 / 3);
    near(b.bonuses["A"], 30);
    near(b.bonuses["C"], 30);
  });
  test("zero_damage_has_no_reward", [] {
    auto b = resolve_batch({{"A", 100}, {"B", 10}, {"C", 100}},
                           {{"A", "B", 0, true}, {"C", "B", 20, true}});
    near(b.bonuses["A"], 0);
    near(b.bonuses["C"], 60);
  });
  test("previous_tick_attacker_excluded", [] {
    auto a = resolve_batch({{"A", 100}, {"B", 20}, {"C", 100}},
                           {{"A", "B", 10, true}});
    auto b = resolve_batch(a.hp, {{"C", "B", 10, true}});
    near(b.bonuses["A"], 0);
    near(b.bonuses["C"], 60);
  });
  test("mutual_kill_award_then_deduct", [] {
    auto b = resolve_batch({{"A", 20}, {"B", 20}},
                           {{"A", "B", 20, true}, {"B", "A", 20, true}});
    check(b.killed.size() == 2, "not simultaneous");
    near(projected_damage(0, b.actual_attack["A"], b.bonuses["A"], 1), 20);
    near(projected_damage(80, b.actual_attack["B"], b.bonuses["B"], 1), 100);
  });
  test("reflection_shares_single_reward_without_attack_credit", [] {
    auto b = resolve_batch({{"A", 100}, {"B", 10}, {"C", 100}},
                           {{"A", "B", 20, true}, {"C", "B", 20, false}});
    near(b.actual_attack["A"], 5);
    near(b.actual_attack["C"], 0);
    near(b.bonuses["A"], 30);
    near(b.bonuses["C"], 30);
  });
  test("pepper_same_batch_dedup_and_no_attack_credit", [] {
    auto b = resolve_batch(
        {{"A", 100}, {"B", 30}, {"C", 100}},
        {{"A", "B", 50, false}, {"A", "B", 50, false}, {"C", "B", 50, false}});
    near(b.actual_attack["A"], 0);
    near(b.actual_attack["C"], 0);
    near(b.bonuses["A"], 30);
    near(b.bonuses["C"], 30);
  });
  test("pepper_mutually_lethal_lines_still_share_batch", [] {
    auto b = resolve_batch({{"A", 40}, {"B", 40}},
                           {{"A", "B", 50, false}, {"B", "A", 50, false}});
    near(b.bonuses["A"], 60);
    near(b.bonuses["B"], 60);
    near(projected_damage(80, 0, b.bonuses["A"], 1), 80);
  });
  test("combat_dead_not_awarded_again_in_pepper", [] {
    auto a = resolve_batch({{"A", 100}, {"B", 20}}, {{"A", "B", 20, true}});
    auto b = resolve_batch(a.hp, {{"A", "B", 50, false}});
    near(b.bonuses["A"], 0);
    check(b.killed.empty(), "duplicate death");
  });
  test("already_dead_source_does_not_trigger_new_fire", [] {
    auto b = resolve_batch({{"A", 0}, {"B", 40}}, {{"A", "B", 50, false}});
    near(b.hp["B"], 40);
    check(b.bonuses.empty(), "dead fire source rewarded");
  });
  test("environment_has_no_kill_award", [] {
    auto b = resolve_batch({{"A", 100}, {"B", 100}}, {});
    check(b.bonuses.empty(), "invented environmental killer");
    near(projected_damage(80, 0, 0, 1), 20);
    near(projected_damage(40, 0, 0, 1), 0);
  });
  test("basic_attack_nearest_protected_blocks", [] {
    auto f = base();
    f.entities = {car("B", {6, 5}, 100, false, true), car("C", {7, 5}, 10)};
    auto e = assess_tactic(f, {}, "attack", "right");
    near(e.attack_credit, 0);
    near(e.bonuses, 0);
  });
  test("attacks_before_move_no_dodge_by_move", [] {
    auto f = base();
    f.hp = 20;
    f.entities = {car("B", {6, 5})};
    check(!assess_tactic(f, {}, "move", "left").alive,
          "movement dodged settled attack");
    check(assess_tactic(f, {}, "defend").alive,
          "defense did not reduce ordinary hit");
  });
  test("purity_defense_can_earn_kill_without_raw_credit", [] {
    auto f = base();
    f.entities = {car("B", {6, 5}, 20)};
    auto e = assess_tactic(f, event("purity", {{"damage", 20}}), "defend");
    near(e.hp, 100);
    near(e.attack_credit, 0);
    near(e.bonuses, 60);
    near(e.damage, 60);
  });
  test("purity_attack_risk_uses_last_defense_as_uncertain", [] {
    auto f = base();
    f.hp = 20;
    f.effective_damage = 160;
    f.entities = {car("B", {6, 5}, 100, true)};
    auto e =
        assess_tactic(f, event("purity", {{"damage", 20}}), "attack", "right");
    check(!e.alive, "reflection lethal branch missed");
    near(e.attack_credit, 0);
    near(e.damage, 100);
  });
  test("pepper_hp_loss_never_ordinary_credit", [] {
    auto f = base();
    f.entities = {car("B", {6, 8}, 40)};
    auto e = assess_tactic(
        f, event("pepper", {{"points", Json::array({{6, 5}})}, {"damage", 50}}),
        "move", "right");
    near(e.attack_credit, 0);
    near(e.pepper_bonus, 60);
    near(e.damage, 60);
    near(e.hp, 100);
  });
  test("pepper_nonlethal_does_not_add_score", [] {
    auto f = base();
    f.entities = {car("B", {6, 8}, 80)};
    auto e = assess_tactic(
        f, event("pepper", {{"points", Json::array({{6, 5}})}, {"damage", 50}}),
        "move", "right");
    near(e.pepper_bonus, 0);
    near(e.damage, 0);
  });
  test("pepper_wait_or_protected_target_no_reward", [] {
    auto f = base();
    f.entities = {car("B", {6, 8}, 40, false, true)};
    auto event_data =
        event("pepper", {{"points", Json::array({{6, 5}})}, {"damage", 50}});
    near(assess_tactic(f, event_data, "move", "right").pepper_bonus, 0);
    near(assess_tactic(f, event_data, "wait").pepper_bonus, 0);
  });
  test("dead_before_movement_cannot_trigger_pepper", [] {
    auto f = base();
    f.hp = 20;
    f.entities = {car("B", {6, 5}), car("C", {5, 8}, 40)};
    auto e = assess_tactic(
        f, event("pepper", {{"points", Json::array({{5, 6}})}, {"damage", 50}}),
        "move", "down");
    check(!e.alive, "lethal combat ignored");
    near(e.pepper_bonus, 0);
  });
  test("damage_mission_above500_and_completed_id_persists", [] {
    auto f = base();
    f.effective_damage = 510;
    f.missions = {{{"id", "d"}, {"type", "effective_damage"}, {"target", 650}}};
    check(needs_damage(f), "ignored task above cap");
    f.completed.insert("d");
    f.effective_damage = 10;
    check(!needs_damage(f), "revoked completed damage task");
  });
  test("mission_utility_only_if_survives_after_deduction", [] {
    auto f = base();
    f.hp = 20;
    f.effective_damage = 80;
    f.entities = {car("B", {6, 5}, 20)};
    auto before = assess_tactic(f, {}, "attack", "right");
    f.missions = {{{"id", "d"}, {"type", "effective_damage"}, {"target", 100}}};
    auto after = assess_tactic(f, {}, "attack", "right");
    check(!after.alive, "mutual lethal ignored");
    near(after.utility, before.utility);
  });
  test("prediction_never_mutates_authoritative_score", [] {
    auto f = base();
    f.score = {{"effective_damage", 100.5}};
    f.effective_damage = 100.5;
    f.entities = {car("B", {6, 5}, 20)};
    assess_tactic(f, {}, "attack", "right");
    near(f.effective_damage, 100.5);
    near(f.score["effective_damage"], 100.5);
  });
  std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
