/* 文件：tests/test_report.cpp
 * 作用：用户报告逐条回归；手算案例验证收益比较、事件时序、降级与边界。
 * 仅离线测试，不用于参赛决策；每项打印 PASS/FAIL。
 */
#include "rmtc_bot/audit.hpp"
#include "rmtc_bot/config.hpp"
#include "rmtc_bot/controller.hpp"
#include "rmtc_bot/gateway.hpp"
#include "rmtc_bot/tactics.hpp"
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
  test("healthy_multi_enemy_does_not_pay_speculative_wait_penalty", [] {
    for (int hp : {60,80,100}) {
      auto j=sample(20);j["self"]["hp"]=hp;
      j["visible_entities"]=Json::array({enemy(-1,0),enemy(0,-1,100,false,false,"C")});
      auto f=parse_frame(j.dump(),"A");Events e;
      auto now=assess_tactic(f,e,"attack","left",false);
      auto future=assess_tactic(f,e,"attack","left",true);
      check(now.utility==future.utility && future.future_risk==0,
            "healthy attack charged speculative next-tick WAIT damage");
    }
  });
  test("patrol_does_not_erase_unproductive_pursuit", [] {
    World w(20,20);Events e;Planner p(2,500,"balanced");
    for (int t=20;t<=28;++t) {
      auto j=sample(t);j["visible_entities"].push_back(enemy(3,0));
      auto f=parse_frame(j.dump(),"A");w.update(f,e);
      if(t>20) p.reason=(t%2 ? "combat_score_goal" : "patrol");
      p.decide(f,w,e);
    }
    check(p.reason=="explore_frontier","alternating patrol erased pursuit cooldown");
  });
  test("third_report_escape_rotations_health_and_score", [] {
    for (int hp : {25,30,40}) for (double damage : {0.,520.})
      for (int range : {1,2}) for (Point origin : {Point{4,4},Point{8,8},Point{15,15}})
      for (auto direction : directions) {
        auto delta=direction.second;delta.first*=range;delta.second*=range;
        auto j=sample(20,"s1",origin);
        j["self"]["hp"]=hp;j["score"]["effective_damage"]=damage;
        j["score"]["resource_value"]=120;
        j["visible_entities"].push_back(enemy(delta.first,delta.second));
        auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
        for(int x=0;x<20;++x)for(int y=0;y<20;++y){w.seen.insert({x,y});w.visits[{x,y}]=1;}
        Planner p(2,500,"balanced");auto a=p.decide(f,w,e);
        check(a["action"]=="move","low HP stayed in fire line");
        auto d=directions.at(a["direction"]);Point dest={origin.first+d.first,origin.second+d.second};
        check(attack_direction({origin.first+delta.first,origin.second+delta.second},dest).empty(),"move stayed in line");
      }
  });
  test("third_report_healing_after_current_combat", [] {
    auto j=sample(20);j["self"]["hp"]=30;
    j["visible_entities"].push_back(enemy(-1,0));
    j["visible_entities"].push_back(entity("resource_high",1,0,"h",20));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
    e.ingest(output(Json::array({{{"kind","angel"},{"start_tick",10},{"end_tick",30},
      {"points",Json::array({{8,9}})},{"effect_ticks",10},{"heal_per_tick",10}}})),20);
    Planner p(2,500,"balanced");auto a=p.decide(f,w,e);
    check(a["action"]=="move" && a["direction"]=="down","ignored post-combat healing/safe point");
  });
  test("third_report_search_line_and_diagonal_distinguished", [] {
    auto j=sample(20);j["visible_entities"].push_back(enemy(-1,0));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
    Planner p(2,500,"balanced");auto paths=p.search(f,w,e);
    check(paths.cost.at({9,8})>paths.cost.at({8,9}),"diagonal incorrectly has same normal fire risk");
  });
  test("third_report_future_risk_respects_kills_protection_and_boost_geometry", [] {
    auto j=sample(20);j["self"]["hp"]=30;j["visible_entities"].push_back(enemy(-1,0,20));
    auto f=parse_frame(j.dump(),"A");Events e;
    auto kill=assess_tactic(f,e,"attack","left",true);
    check(kill.alive && kill.future_risk==0,"dead enemy threatens next tick");
    j["visible_entities"]=Json::array({enemy(-1,0,100,true)});f=parse_frame(j.dump(),"A");
    check(assess_tactic(f,e,"wait","",true).future_risk==0,"protected enemy creates damage");
    j["visible_entities"]=Json::array({enemy(-1,-1)});f=parse_frame(j.dump(),"A");
    check(!threatens(f.entities.front(),{8,8},e,20),"normal diagonal is attackable");
    e.ingest(output(Json::array(),Json::array({{{"kind","unrivaled"},{"team","B"},
      {"start_tick",0},{"end_tick",30},{"damage",50}}})),20);
    check(threatens(f.entities.front(),{8,8},e,20),"boost diagonal incorrectly safe");
    check(!threatens(f.entities.front(),{8,8},e,30),"expired boost still threatens diagonal");
  });
  test("third_report_last_tick_has_no_imaginary_future", [] {
    auto j=sample(499);j["self"]["hp"]=30;j["visible_entities"].push_back(enemy(-1,0));
    auto f=parse_frame(j.dump(),"A");Events e;World w(20,20);w.update(f,e);
    for(int x=0;x<20;++x)for(int y=0;y<20;++y){w.seen.insert({x,y});w.visits[{x,y}]=1;}
    Planner p(2,500,"balanced");check(p.decide(f,w,e)["action"]=="attack","retreat for nonexistent tick500 combat");
  });
  test("third_report_fortress_end_exclusive_requires_attack_window", [] {
    for(int end : {11,12}) {
      auto f=frame(10);Events e;World w(20,20);w.update(f,e);
      for(int x=0;x<20;++x)for(int y=0;y<20;++y){w.seen.insert({x,y});w.visits[{x,y}]=1;}
      e.ingest(output(Json::array({{{"kind","unrivaled"},{"start_tick",10},{"end_tick",end},
        {"center",{8,8}},{"size",3},{"stay_ticks",1}}})),10);
      Planner p(2,500,"balanced");p.decide(f,w,e);
      check((p.reason=="hold_announced_fortress")== (end==12),"fortress completion without a usable attack tick valued");
    }
  });
  test("third_report_healthy_can_attack_and_lethal_move_cannot_escape", [] {
    for(int hp : {100,15}) {
      auto j=sample(20);j["self"]["hp"]=hp;j["visible_entities"].push_back(enemy(-1,0));
      auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
      for(int x=0;x<20;++x)for(int y=0;y<20;++y){w.seen.insert({x,y});w.visits[{x,y}]=1;}
      Planner p(2,500,"balanced");auto a=p.decide(f,w,e);
      check(a["action"]==(hp==100?"attack":"defend"),"healthy over-retreat or pre-move death ignored");
    }
  });

  test("second_report_nonlethal_blast_not_discounted_by_depth", [] {
    auto j = sample(10, "s1", {10,10});
    j["visible_entities"].push_back(entity("resource_high", 0, 2, "h", 20));
    j["missions"].push_back({{"id","h"},{"type","high_resources"},{"target",1}});
    auto f = parse_frame(j.dump(), "A"); World w(20,20); Events e; w.update(f,e);
    for(int x=0;x<20;++x) for(int y=0;y<20;++y) {
      w.seen.insert({x,y}); w.visits[{x,y}]=1;
      if(x!=10 || y<8 || y>12) w.obstacles[{x,y}]=0;
    }
    e.ingest(output(Json::array({{{"kind","explosion"},{"start_tick",12},
      {"end_tick",13},{"rect",{10,12,10,12}},{"damage",80}}})),10);
    Planner p(2,500,"balanced"); auto a=p.decide(f,w,e);
    check(!(a["action"]=="move" && a["direction"]=="down"), "80HP risk still hidden by depth discount");
  });
  test("second_report_two_step_escape_without_other_goals", [] {
    auto f=frame(10,"s1",{10,10}); World w(20,20); Events e; w.update(f,e);
    for(int x=0;x<20;++x) for(int y=0;y<20;++y) {w.seen.insert({x,y});w.visits[{x,y}]=1;}
    e.ingest(output(Json::array({{{"kind","explosion"},{"start_tick",12},
      {"end_tick",13},{"rect",{9,9,11,11}},{"damage",80}}})),10);
    Planner p(2,500,"balanced");
    for(int tick=10;tick<12;++tick) {
      f.tick=tick;auto a=p.decide(f,w,e); check(a["action"]=="move","waited until too late");
      auto d=directions.at(a["direction"].get<std::string>());f.position.first+=d.first;f.position.second+=d.second;
    }
    check(!inside(f.position,Json::array({9,9,11,11})),"two steps still inside explosion");
  });
  test("second_report_full_combat_explores_instead_of_chasing", [] {
    auto j=sample(20);j["score"]["effective_damage"]=520;j["visible_entities"].push_back(enemy(3,0));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);Planner p(2,500,"balanced");p.decide(f,w,e);
    check(p.reason=="explore_frontier","full combat still chases instead of exploring");
  });
  test("second_report_protected_enemy_not_chased", [] {
    auto j=sample(20);j["visible_entities"].push_back(enemy(3,0,100,true));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);Planner p(2,500,"balanced");p.decide(f,w,e);
    check(p.reason=="explore_frontier","protected enemy still chased");
  });
  test("second_report_protected_enemy_does_not_force_defense", [] {
    auto j=sample(20);j["visible_entities"].push_back(enemy(1,0,100,true));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
    for(auto row:directions)w.obstacles[{8+row.second.first,8+row.second.second}]=0;
    Planner p(2,500,"balanced");check(p.decide(f,w,e)["action"]=="wait","blind defense against protected enemy");
  });
  test("second_report_critical_healing_competes_with_resource", [] {
    auto j=sample(20);j["self"]["hp"]=20;j["visible_entities"].push_back(entity("resource_high",1,0,"h",20));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
    e.ingest(output(Json::array({{{"kind","angel"},{"start_tick",10},{"end_tick",30},
      {"points",Json::array({{8,9}})},{"effect_ticks",10},{"heal_per_tick",10}}})),20);
    Planner p(2,500,"balanced");auto a=p.decide(f,w,e);
    check(a["action"]=="move" && a["direction"]=="down","critical health ignores available healing");
  });
  test("second_report_healthy_resource_still_beats_healing", [] {
    auto j=sample(20);j["self"]["hp"]=90;j["visible_entities"].push_back(entity("resource_high",1,0,"h",20));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
    e.ingest(output(Json::array({{{"kind","angel"},{"start_tick",10},{"end_tick",30},
      {"points",Json::array({{8,9}})},{"effect_ticks",10},{"heal_per_tick",10}}})),20);
    Planner p(2,500,"balanced");auto a=p.decide(f,w,e);
    check(a["action"]=="move" && a["direction"]=="right","healing overvalued when nearly healthy");
  });
  test("second_report_fortress_progress_not_restarted_each_tick", [] {
    auto j=sample(10);j["visible_entities"].push_back(entity("resource_high",2,0,"h",20));j["visible_entities"].push_back(enemy(4,4));
    auto f=parse_frame(j.dump(),"A");World w(20,20);Events e;w.update(f,e);
    for(int t=3;t<=10;++t)w.history[t]={{8,8},true};
    e.ingest(output(Json::array({{{"kind","unrivaled"},{"start_tick",0},{"end_tick",50},
      {"center",{8,8}},{"size",3},{"stay_ticks",10},{"damage",50}}})),10);
    Planner p(2,500,"balanced");p.decide(f,w,e);
    check(p.reason=="hold_announced_fortress","nearly earned fortress abandoned for another resource");
  });
  test("second_report_provisional_model_not_ingested", [] {
    Controller c("A",2,500);c.handle(frame(5));auto parsed=output(Json::array({{{"kind","explosion"},
      {"start_tick",10},{"end_tick",11},{"rect",{1,1,2,2}}}}));
    check(!c.ingest({{"session_id","s1"},{"source_tick",5},{"parsed",parsed},{"provisional",true}},5),"provisional facts entered controller");
    check(c.events.events.empty(),"provisional event retained");
  });

  test("stalled_pursuit_yields_to_exploration_then_allows_attack", [] {
    World w(20,20); Events e; Planner p(2,500,"balanced");
    for (int t=20;t<=28;++t) {
      auto j=sample(t); j["visible_entities"].push_back(enemy(3,0));
      auto f=parse_frame(j.dump(),"A"); w.update(f,e); p.decide(f,w,e);
    }
    check(p.reason=="explore_frontier", "no-progress pursuit repeats indefinitely");
    auto j=sample(29); j["visible_entities"].push_back(enemy(1,0));
    auto f=parse_frame(j.dump(),"A"); w.update(f,e);
    check(p.decide(f,w,e)["action"]=="attack", "pursuit cooldown must not block actual attacks");
  });
  test("damage_task_does_not_circle_attackable_enemy", [] {
    for (int range : {1, 2}) {
      Controller c("A", 2, 500);
      auto j = sample(20);
      j["visible_entities"].push_back(enemy(range, 0));
      j["missions"].push_back({{"id", "damage"},
                                {"type", "effective_damage"},
                                {"target", 80.5}});
      auto a = c.handle(parse_frame(j.dump(), "A")).first;
      check(a["action"] == "attack" && a["direction"] == "right",
            "already in range but approach task reward keeps circling");
    }
  });
  test("balanced_compares_resource_task_with_attack", [] {
    Controller c("A", 2, 500);
    auto j = sample(20);
    j["visible_entities"].push_back(enemy(1, 0));
    j["visible_entities"].push_back(entity("resource_high", 0, 1, "h", 20));
    j["missions"].push_back(
        {{"id", "high"}, {"type", "high_resources"}, {"target", 1}});
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "move" && a["direction"] == "down",
          "attack preempted high-value collection");
  });
  test("defense_competes_with_distant_resource", [] {
    auto j = sample(20);
    j["visible_entities"].push_back(enemy(2, 2));
    j["visible_entities"].push_back(entity("resource_normal", 0, -2));
    auto f = parse_frame(j.dump(), "A");
    World w(20, 20);
    Events e;
    auto v = output(Json::array(), Json::array({{{"kind", "unrivaled"},
                                                 {"team", "B"},
                                                 {"start_tick", 0},
                                                 {"end_tick", 40},
                                                 {"damage", 50},
                                                 {"defended_damage", 10}}}));
    e.ingest(v, 20);
    w.update(f, e);
    Planner p(2, 500, "balanced");
    auto a = p.decide(f,w,e);
    // 一步前瞻允许放弃即时防御、离开无双范围；不能朝仍受攻击的资源移动。
    if (a["action"] == "move") {
      auto d=directions.at(a["direction"]);
      check(std::max(std::abs(8+d.first-10),std::abs(8+d.second-10))>2,
            "resource move remains in boosted fire");
    } else check(a["action"]=="defend","neither defended nor left boosted fire");
  });
  test("last_tick_adjacent_resource", [] {
    Controller c;
    auto j = sample(99);
    j["visible_entities"].push_back(entity("resource_high", 1, 0, "h", 20));
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "move" && a["direction"] == "right",
          "missed final resource");
  });
  test("last_tick_adjacent_reach", [] {
    Controller c("A", 2, 500);
    auto j = sample(499);
    j["missions"].push_back(
        {{"id", "r"}, {"type", "reach"}, {"position", {9, 8}}});
    auto a = c.handle(parse_frame(j.dump(), "A")).first;
    check(a["action"] == "move" && a["direction"] == "right",
          "missed final reach");
  });
  test("weighted_cost_is_not_event_time", [] {
    auto f = frame(9);
    f.width = 4;
    f.height = 2;
    f.position = {0, 0};
    f.hp = 50;
    Entity b;
    b.type = "robot";
    b.team = "B";
    b.xy = {1, 1};
    b.hp = 100;
    f.entities = {b};
    World w(4, 2);
    Events e;
    w.update(f, e);
    for (int x = 0; x < 4; ++x)
      w.obstacles[{x, 1}] = 0;
    e.ingest(output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 11},
                                  {"end_tick", 12},
                                  {"rect", {2, 0, 2, 0}},
                                  {"damage", 80}}})),
             9);
    Planner p(2, 500, "safe");
    auto paths = p.search(f, w, e);
    check(!paths.cost.count({2, 0}), "weighted clock missed lethal explosion");
  });
  test("path_accumulates_announced_damage", [] {
    auto f = frame(9);
    f.width = 4;
    f.height = 1;
    f.position = {0, 0};
    World w(4, 1);
    Events e;
    w.update(f, e);
    e.ingest(output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 10},
                                  {"end_tick", 11},
                                  {"rect", {1, 0, 1, 0}},
                                  {"damage", 60}},
                                 {{"kind", "explosion"},
                                  {"start_tick", 11},
                                  {"end_tick", 12},
                                  {"rect", {2, 0, 2, 0}},
                                  {"damage", 60}}})),
             9);
    Planner p(2, 500, "balanced");
    check(!p.search(f, w, e).cost.count({2, 0}),
          "two nonlethal hits form lethal route");
  });
  test("temporary_block_expires_before_arrival", [] {
    auto f = frame(10);
    f.width = 5;
    f.height = 1;
    f.position = {0, 0};
    World w(5, 1);
    Events e;
    w.update(f, e);
    w.blocked_until[{3, 0}] = 12;
    Planner p(2, 500, "balanced");
    check(p.search(f, w, e).cost.count({4, 0}),
          "temporary blockage treated permanent");
  });
  test("state_error_does_not_invent_obstacle", [] {
    World w(20, 20);
    Events e;
    auto f = frame(1);
    w.update(f, e);
    w.record(f, action(f, "move", "right"));
    auto j = sample(2);
    j["self"]["last_action"] = {{"tick", 1},
                                {"action", "move"},
                                {"moved", false},
                                {"error", "E_STATE"}};
    w.update(parse_frame(j.dump(), "A"), e);
    check(w.blocked_until.empty(), "state error became blocked cell");
  });
  test("missing_target_mission_isolated", [] {
    Controller c("A", 2, 500);
    auto j = sample();
    j["missions"].push_back({{"id", "rv"}, {"type", "resource_value"}});
    j["visible_entities"].push_back(entity("resource_normal", 1, 0));
    check(c.handle(parse_frame(j.dump(), "A")).first["action"] == "move",
          "malformed mission stopped base strategy");
  });
  test("model_exact_integral_float", [] {
    auto j = output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 10.0},
                                  {"end_tick", 11.0},
                                  {"rect", {0.0, 0, 1, 1}}}}));
    j["goal"] = {12.0, 12};
    check(model_output(j.dump(), "s1", 20, 20)["events"].size() == 1,
          "integral float rejected");
  });
  test("model_fence_whitespace_crlf", [] {
    check(model_output(" \r\n```\r\n" + output().dump() + "\r\n``` \n", "s1",
                       20, 20)["session_id"] == "s1",
          "format rejected");
  });
  test("model_partial_valid_events_preserved", [] {
    auto j = output(
        Json::array({{{"kind", "explosion"},
                      {"start_tick", 10},
                      {"end_tick", 11},
                      {"rect", {0, 0, 1, 1}}},
                     {{"kind", "bad"}, {"start_tick", 1}, {"end_tick", 2}}}));
    j["goal"] = {99, 99};
    check(model_output(j.dump(), "s1", 20, 20)["events"].size() == 1,
          "one bad item discarded good event");
  });
  test("pan_missing_window_does_not_extend_to30", [] {
    Events e;
    e.ingest(output(Json::array(
                 {{{"kind", "pan"}, {"start_tick", 10}, {"end_tick", 40}}})),
             10);
    e.infer_gains({}, {{35, {1, 1}}}, "A", 36);
    check(e.effect("vulnerable", "A", 36).empty(),
          "guessed longer acquisition");
  });
  test("angel_effect_includes_trigger_tick", [] {
    Events e;
    e.ingest(output(Json::array({{{"kind", "angel"},
                                  {"start_tick", 10},
                                  {"end_tick", 30},
                                  {"points", Json::array({{2, 2}})},
                                  {"effect_ticks", 10}}})),
             10);
    e.infer_gains({}, {{12, {2, 2}}}, "A", 13);
    check(!e.effect("angel", "A", 12).empty() &&
              e.effect("angel", "A", 22).empty(),
          "healing not ten ticks");
  });
  test("successful_move_before_death_kept", [] {
    World w(20, 20);
    Events e;
    auto j = sample(2);
    j["self"]["alive"] = false;
    j["self"]["hp"] = 0;
    j["self"]["last_action"] = {{"tick", 1},
                                {"action", "move"},
                                {"moved", true},
                                {"effective_direction", "right"}};
    w.update(parse_frame(j.dump(), "A"), e);
    check(w.successful_moves.count(1), "lost successful move after death");
  });
  test("explosion_one_activation_tick", [] {
    Events e;
    auto j = output(Json::array({{{"kind", "explosion"},
                                  {"start_tick", 10},
                                  {"end_tick", 20},
                                  {"rect", {1, 1, 2, 2}},
                                  {"damage", 80}}}));
    e.ingest(model_output(j.dump(), "s1", 20, 20), 10);
    check(e.danger({1, 1}, 11) == 0, "one-shot treated sustained");
  });
  test("stale_goal_uses_latest_controller_tick", [] {
    Controller c;
    c.session = "s1";
    c.tick = 30;
    auto j = output();
    j["goal"] = {3, 3};
    c.ingest({{"session_id", "s1"}, {"source_tick", 14}, {"parsed", j}}, 25);
    check(c.events.goal.first < 0, "old frame revived stale goal");
  });

  test("deep_json_rejected_without_recursion_crash", [] {
    rejects(
        [] { strict_json(std::string(80, '[') + "0" + std::string(80, ']')); });
  });
  test("log_directory_failure_is_degraded_not_fatal", [] {
    Audit a("/proc/rmtc-unwritable-audit-test");
    a.write({{"kind", "test"}});
    a.close();
    check(a.error.load(), "log loss not reported");
  });
  test("fortress_corner_counts_as_inside", [] {
    auto f = frame(10);
    World w(20, 20);
    Events e;
    w.update(f, e);
    e.ingest(output(Json::array({{{"kind", "unrivaled"},
                                  {"start_tick", 1},
                                  {"end_tick", 50},
                                  {"center", {9, 9}},
                                  {"size", 3},
                                  {"stay_ticks", 10}}})),
             10);
    Planner p(2, 500, "balanced");
    check(p.decide(f, w, e)["action"] == "defend", "3x3 corner not recognized");
  });
  test("expired_angel_acquisition_not_chased", [] {
    auto f = frame(20);
    f.hp = 40;
    World w(20, 20);
    Events e;
    w.update(f, e);
    e.ingest(output(Json::array({{{"kind", "angel"},
                                  {"start_tick", 1},
                                  {"end_tick", 50},
                                  {"acquire_ticks", 5},
                                  {"points", Json::array({{9, 8}})}}})),
             20);
    Planner p(2, 500, "balanced");
    p.decide(f, w, e);
    check(p.reason != "seek_announced_healing", "chasing closed acquisition");
  });
  std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
