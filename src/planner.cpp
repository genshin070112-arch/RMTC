/*
 * 文件：src/planner.cpp
 * 作用：动作规划与策略（实现文件）
 * 用加权最短路、资源/任务收益、战斗和危险代价，选择本 Tick 的动作。
 * 输入 Frame、World、Events 与策略枚举；输出动作。由 controller
 * 调用，不等待模型请求。
 * 配套文件：include/rmtc_bot/planner.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/planner.hpp"
#include "rmtc_bot/tactics.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
namespace rmtc {
const std::set<std::string> strategies = {"balanced", "collect", "combat",
                                          "safe"};
Search Planner::search(const Frame &f, const World &w,
                       const Events &events) const {
  Search out;
  out.cost[f.position] = 0;
  out.depth[f.position] = 0;
  out.damage[f.position] = 0;
  out.first[f.position] = "";
  using Item = std::pair<double, Point>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
  heap.push({0, f.position});
  std::set<Point> occupied;
  for (const auto &e : w.enemies)
    occupied.insert(e.xy);
  while (!heap.empty()) {
    auto [cost, p] = heap.top();
    heap.pop();
    if (cost > out.cost.at(p))
      continue;
    for (const auto &[dir, delta] : directions) {
      Point q = {p.first + delta.first, p.second + delta.second};
      if (q.first < 0 || q.first >= f.width || q.second < 0 ||
          q.second >= f.height || w.obstacles.count(q) || occupied.count(q))
        continue;
      int depth = out.depth.at(p) + 1;
      int arrival = f.tick + depth - 1; // 走入q所用动作的Tick
      if (depth > max_ticks - f.tick)
        continue;
      auto blocked = w.blocked_until.find(q);
      if (blocked != w.blocked_until.end() && arrival < blocked->second)
        continue;
      if (events.blocked(q, arrival, w.spawn))
        continue;
      double risk = events.danger(q, arrival + 1, false);
      if (out.damage.at(p) + risk >= f.hp)
        continue;
      bool vulnerable = !events.effect("vulnerable", f.seat, f.tick).empty();
      int near = 0;
      if (stage != 1)
        for (const auto &e : w.enemies)
          if (threatens(e, q, events, arrival + 1))
            ++near;
      int visits = w.visits.count(q) ? w.visits.at(q) : 0;
      double step =
          1 + (w.seen.count(q) ? 0 : .12) + std::min(visits, 5) * .025;
      step +=
          near *
              ((strategy == "safe" || f.hp <= 35 || vulnerable) ? 1.2 : .12) +
          risk * (.1 + std::min(f.effective_damage, death_deduction) / 300.0);
      double next = cost + step;
      auto it = out.cost.find(q);
      if (it == out.cost.end() || next < it->second) {
        out.cost[q] = next;
        out.depth[q] = depth;
        out.damage[q] = out.damage.at(p) + risk;
        out.first[q] = (p == f.position ? dir : out.first.at(p));
        heap.push({next, q});
      }
    }
  }
  return out;
}
Json Planner::move(const Frame &f, std::string dir,
                   const Events &events) const {
  if (!events.effect("illusion", f.seat, f.tick).empty())
    dir = opposite(dir);
  return action(f, "move", dir, stage);
}
Json Planner::decide(const Frame &f, const World &w, const Events &events) {
  if (stage != 1 && f.tick > last_decision_tick_) {
    const bool pursuing = reason == "combat_score_goal" || reason == "mission_damage";
    // 接近与巡逻交替仍是同一段未兑现接敌；实际伤害进展或其他目标重置。
    const bool stalled_patrol = reason == "patrol" && pursuit_stalled_ > 0;
    if ((pursuing || stalled_patrol) && f.tick == last_decision_tick_ + 1 &&
        f.effective_damage <= last_damage_)
      ++pursuit_stalled_;
    else
      pursuit_stalled_ = 0;
    if (pursuit_stalled_ >= 8) {
      pursuit_until_ = f.tick + 12;
      pursuit_stalled_ = 0;
    }
    last_damage_ = f.effective_damage;
    last_decision_tick_ = f.tick;
  }
  reason = "wait";
  goal = {-1, -1};
  if (f.tick >= max_ticks)
    return Json();
  if (!f.alive)
    return action(f, "wait", "", stage);
  auto assess = [&](const std::string &a, const std::string &d = "") {
    return assess_tactic(f, events, a, d, f.tick + 1 < max_ticks);
  };
  Search paths = search(f, w, events);
  // 公告在动作前生效：必须在 start_tick 前完成整条撤离路线。
  // 到最短撤离路程仅剩一Tick余量时开始离开，不能等到下一Tick才检查。
  int deadline = max_ticks;
  for (const auto &row : events.events) {
    const auto &e = row.second;
    int start = e.at("start_tick");
    if (start <= f.tick || start >= deadline)
      continue;
    const bool blast = e.at("kind") == "explosion" &&
                       inside(f.position, e.at("rect"));
    const bool split = e.at("kind") == "world_split" &&
                       f.position != w.spawn &&
                       (f.position.first == e.at("col") ||
                        f.position.second == e.at("row"));
    if (blast || split)
      deadline = start;
  }
  if (deadline < max_ticks) {
    Point exit = {-1, -1};
    double best_cost = std::numeric_limits<double>::infinity();
    int min_steps = max_ticks;
    for (const auto &row : paths.cost) {
      Point p = row.first;
      const int steps = paths.depth.at(p);
      if (steps == 0 || steps > deadline - f.tick ||
          events.danger(p, deadline) > 0 || events.blocked(p, deadline, w.spawn))
        continue;
      min_steps = std::min(min_steps, steps);
      if (row.second < best_cost) {
        best_cost = row.second;
        exit = p;
      }
    }
    if (exit.first >= 0 && deadline - f.tick <= min_steps + 1) {
      goal = exit;
      reason = "escape_announced_event";
      return move(f, paths.first.at(exit), events);
    }
  }
  if (events.danger(f.position, f.tick + 1, f.protected_spawn) > 0) {
    std::vector<std::pair<double, std::string>> exits;
    for (const auto &[dir, d] : directions) {
      Point p = {f.position.first + d.first, f.position.second + d.second};
      if (paths.first.count(p) && paths.first.at(p) == dir)
        exits.push_back(
            {events.danger(p, f.tick + 1) + events.entry_penalty(p, f.tick),
             dir});
    }
    if (!exits.empty()) {
      reason = "escape_announced_event";
      return move(f, std::min_element(exits.begin(), exits.end())->second,
                  events);
    }
  }
  // 即时战斗和目标移动在同一分值尺度比较。HP成本是显式生存偏好，不是裁判计分。
  Json tactical_action = action(f, "wait", "", stage);
  double tactical_utility = -1e30;
  std::string tactical_reason = "wait";
  TacticalEstimate waiting;
  bool has_damage_action = false;
  if (stage != 1) {
    waiting = assess("wait");
    tactical_utility = waiting.utility;
    auto consider = [&](const TacticalEstimate &t, const std::string &a,
                        const std::string &d, const std::string &why) {
      if (t.utility > tactical_utility + 1e-9) {
        tactical_utility = t.utility;
        tactical_action =
            a == "move" ? move(f, d, events) : action(f, a, d, stage);
        tactical_reason = why;
      }
    };
    const auto defense = assess("defend");
    consider(defense, "defend", "",
             defense.bonuses > 0
                 ? "purity_defense_kill_opportunity"
                 : (!waiting.alive && defense.alive ? "avoid_death_deduction"
                                                    : "hold_defense"));
    if (!f.protected_spawn)
      for (const auto &[dir, delta] : directions) {
        (void)delta;
        auto t = assess("attack", dir);
        if (t.attack_credit + t.bonuses > 0) {
          has_damage_action = true;
          consider(t, "attack", dir, "visible_enemy_attack");
        }
      }
    for (const auto &[dir, delta] : directions) {
      Point p = {f.position.first + delta.first,
                 f.position.second + delta.second};
      if (!paths.first.count(p) || paths.first.at(p) != dir)
        continue;
      auto t = assess("move", dir);
      if (t.alive && events.danger(p, f.tick + 1) < t.hp &&
          (t.pepper_bonus > 0 || t.future_risk < waiting.future_risk))
        consider(t, "move", dir, t.pepper_bonus > 0 ? "pepper_kill_opportunity"
                                                    : "leave_visible_fire_line");
    }
  }
  struct Candidate {
    double utility;
    Point p;
    std::string reason;
  };
  std::vector<Candidate> candidates;
  std::vector<Json> missions;
  for (const auto &m : f.missions)
    if (!f.completed.count(m["id"].get<std::string>()))
      missions.push_back(m);
  int remaining = max_ticks - f.tick;
  for (const auto &row : w.resources) {
    const auto &e = row.second;
    if (!paths.cost.count(e.xy))
      continue;
    double d = paths.cost.at(e.xy);
    if (d <= 0 || paths.depth.at(e.xy) > remaining ||
        paths.first.at(e.xy).empty())
      continue;
    double value = e.value;
    if (stage != 1) {
      const double current = f.score.value("resource_value", 0.0);
      value = 30.0 *
              (std::min(resource_cap, current + e.value) -
               std::min(resource_cap, current)) /
              resource_cap;
      value += .002 * e.value; // 同分资源量的弱排序偏好，避免盖过实际分数。
      for (const auto &m : missions) {
        if ((m["type"] == "normal_resources" && e.type == "resource_normal") ||
            (m["type"] == "high_resources" && e.type == "resource_high") ||
            (m["type"] == "resource_value" &&
             current < m.at("target").get<double>()))
          value += 5; // 任务进度潜在收益；资格仍由正式网关和裁判确认。
      }
      auto seen = w.resource_last_seen.find(e.id);
      const int age =
          seen == w.resource_last_seen.end() ? 0 : f.tick - seen->second;
      value /=
          1.0 + std::max(0, age) / 30.0; // 只降权，不宣称视野外资源已经消失。
      if (strategy == "collect")
        value *= 1.15;
      if (strategy == "combat")
        value *= .85;
    }
    int follow = 100;
    for (const auto &other : w.resources)
      if (other.first != row.first)
        follow = std::min(follow, distance(e.xy, other.second.xy));
    if (follow == 100)
      follow = 8;
    candidates.push_back(
        {value / (stage == 1 ? d + 2 : std::max(1.0, d)) +
             (stage == 1 ? 1.0 : .01) / (follow + 1),
         e.xy, "resource"});
  }
  if (stage != 1) {
    const bool damage_task = needs_damage(f);
    // 已有有效攻击时，不能再次把“靠近攻击位”的未来任务分当成移动收益。
    // 否则每Tick重规划会在射程内绕圈而不兑现伤害；资源/任务/防御仍正常比较。
    if (!has_damage_action && f.tick >= pursuit_until_ &&
        (damage_task || f.effective_damage < combat_full_damage)) {
      for (const auto &enemy : w.enemies) {
        if (enemy.protected_spawn)
          continue;
        for (const auto &[dir, delta] : directions) {
          (void)dir;
          for (int reach = 1; reach <= 2; ++reach) {
            Point p = {enemy.xy.first + delta.first * reach,
                       enemy.xy.second + delta.second * reach};
            if (paths.first.count(p) && !paths.first.at(p).empty() &&
                paths.depth.at(p) < remaining) {
              double weight = damage_task ? 6.2 : 1.2;
              if (strategy == "combat")
                weight *= 1.3;
              if (strategy == "safe" || f.hp <= 35)
                weight *= .35;
              candidates.push_back(
                  {weight / (paths.cost.at(p) + 1.0), p,
                   damage_task ? "mission_damage" : "combat_score_goal"});
            }
          }
        }
      }
    }
    for (const auto &m : missions)
      if (m["type"] == "reach") {
        Point p = point(m["position"], f.width, f.height);
        if (paths.cost.count(p) && !paths.first.at(p).empty() &&
            paths.depth.at(p) <= remaining)
          candidates.push_back(
              {5.0 / std::max(1.0, paths.cost.at(p)), p, "mission_reach"});
      }
    if (paths.first.count(events.goal) && !paths.first.at(events.goal).empty())
      candidates.push_back(
          {.8 / (paths.cost.at(events.goal) + 1.0), events.goal, "llm_goal"});
  }
  if (stage != 1) {
    for (const auto &e : events.active("angel", f.tick))
      {
        for (const auto &raw : e.at("points")) {
          Point p = point(raw, f.width, f.height);
          if (!paths.first.count(p) || paths.first.at(p).empty())
            continue;
          const int depth = paths.depth.at(p);
          const int arrival = f.tick + depth - 1;
          if (arrival >= std::min(e.at("end_tick").get<int>(),
                                  e.at("start_tick").get<int>() +
                                      e.value("acquire_ticks", 20)))
            continue;
          // 按可恢复血量估值，已有回血不重复计分；危急血量加入生存偏好。
          const auto after = assess("move", paths.first.at(p));
          if (!after.alive) continue; // 本Tick战斗死亡后无法入点治疗。
          const double missing = 100 - after.hp;
          if (missing <= 0) continue;
          double existing = 0;
          for (const auto &effect : events.effect("angel", f.seat, f.tick))
            existing = std::max(existing, effect.value("heal_per_tick", 10.0) *
                std::min(remaining, effect.at("end_tick").get<int>() - f.tick));
          const double recover = std::min(std::max(0.0, missing - existing),
              e.value("heal_per_tick", 10.0) *
                  std::min(e.value("effect_ticks", 10), remaining - depth + 1));
          const double survival = 6.0 * std::max(0.0, (40 - after.hp) / 40.0) *
                                  recover / missing;
          if (recover > 0)
            candidates.push_back({(.04 * recover + survival) /
                                      std::max(1.0, paths.cost.at(p)),
                                  p, "seek_announced_healing"});
        }
      }
    if (events.effect("unrivaled", f.seat, f.tick).empty()) {
      for (const auto &e : events.active("unrivaled", f.tick)) {
        Point p = point(e.at("center"), f.width, f.height);
        const int half = e.value("size", 3) / 2;
        auto within = [&](Point q) {
          return std::abs(q.first - p.first) <= half &&
                 std::abs(q.second - p.second) <= half;
        };
        int streak = 0;
        // history[t] 是 t-1 动作后的状态；缺帧/离区/死亡会中断可证实的驻留。
        for (int t = f.tick; t > e.at("start_tick").get<int>(); --t) {
          auto h = w.history.find(t);
          if (h == w.history.end() || !h->second.second || !within(h->second.first))
            break;
          ++streak;
        }
        const int stay = e.value("stay_ticks", 10);
        const int left = std::max(1, stay - streak);
        const int end = std::min(max_ticks, e.at("end_tick").get<int>());
        int targets = 0;
        for (const auto &enemy : w.enemies)
          targets += !enemy.protected_spawn;
        auto benefit = [&](int ticks_to_gain) {
          const double opportunity = std::max(0, end - f.tick - ticks_to_gain);
          const double extra = std::max(0.0, e.value("damage", 50.0) - 20.0);
          // 可见对手与剩余时长提供折扣后的机会估值，并限制在尚缺的分数内。
          const double ceiling = std::max(0.0, 15.0 *
              (combat_full_damage - f.effective_damage) / combat_full_damage) +
              (needs_damage(f) ? 5.0 : 0.0);
          return std::min(ceiling, 3.0 + targets * opportunity * extra * .03 * .25);
        };
        if (within(f.position) && f.tick + left < end) {
          auto hold = assess("defend");
          double value = benefit(left) / left;
          if (hold.alive && hold.utility + value > tactical_utility) {
            tactical_utility = hold.utility + value;
            tactical_action = action(f, "defend", "", stage);
            tactical_reason = "hold_announced_fortress";
          }
        }
        if (paths.first.count(p) && !paths.first.at(p).empty() &&
            f.tick + paths.depth.at(p) + stay - 1 < end) {
          // 入区当Tick也算一次驻留；路径成本用于价值，真实步数用于时序。
          const int travel = paths.depth.at(p);
          candidates.push_back({benefit(travel + stay - 1) /
                                    (paths.cost.at(p) + stay - 1),
                                p, "seek_announced_fortress"});
        }
      }
    }
  }
  // 未见区域可能有资源：使用有上限的探索估值与战斗竞争，避免无收益追逐。
  // 这只是搜索偏好，不假定未知格必有资源；资源分满后不再加该奖励。
  if (stage != 1 && f.score.value("resource_value", 0.0) < resource_cap) {
    const double gap = 1.0 - f.score.value("resource_value", 0.0) / resource_cap;
    const double area = (2 * f.radius + 1) * (2 * f.radius + 1);
    for (const auto &row : paths.cost) {
      Point p = row.first;
      if (paths.first.at(p).empty() || paths.depth.at(p) > remaining)
        continue;
      int unseen = 0;
      for (int x = std::max(0, p.first - f.radius);
           x < std::min(f.width, p.first + f.radius + 1); ++x)
        for (int y = std::max(0, p.second - f.radius);
             y < std::min(f.height, p.second + f.radius + 1); ++y)
          unseen += !w.seen.count({x, y});
      if (unseen)
        candidates.push_back({4.0 * gap * unseen / area / (row.second + 2),
                              p, "explore_frontier"});
    }
  }
  if (stage != 1) {
    for (auto &candidate : candidates) {
      const auto &dir = paths.first.at(candidate.p);
      auto t = assess("move", dir);
      // 所有资源/任务路径都计入本Tick战斗损失，不能靠MOVE躲攻击。
      candidate.utility = (t.alive ? candidate.utility : 0) + t.utility;
      // 绝对HP损失不能被路程摊薄；加权路费同时保留敌人邻近代价。
      candidate.utility -= .04 * paths.damage.at(candidate.p);
    }
    if (!candidates.empty()) {
      const auto best =
          std::max_element(candidates.begin(), candidates.end(),
                           [](const Candidate &a, const Candidate &b) {
                             return a.utility < b.utility;
                           });
      if (best->utility <= tactical_utility) {
        reason = tactical_reason;
        return tactical_action;
      }
    }
  }
  if (!candidates.empty()) {
    const auto &best =
        *std::max_element(candidates.begin(), candidates.end(),
                          [&](const Candidate &a, const Candidate &b) {
                            if (a.utility != b.utility)
                              return a.utility < b.utility;
                            return paths.cost.at(a.p) > paths.cost.at(b.p);
                          });
    goal = best.p;
    reason = best.reason;
  } else {
    if (goal.first < 0) {
      double best = -1;
      for (const auto &row : paths.cost) {
        Point p = row.first;
        double d = row.second;
        if (paths.first.at(p).empty() || paths.depth.at(p) > remaining)
          continue;
        int unseen = 0;
        for (int x = std::max(0, p.first - f.radius);
             x < std::min(f.width, p.first + f.radius + 1); ++x)
          for (int y = std::max(0, p.second - f.radius);
               y < std::min(f.height, p.second + f.radius + 1); ++y)
            if (!w.seen.count({x, y}))
              ++unseen;
        double score = unseen / (d + 4.0);
        if (unseen && score > best) {
          best = score;
          goal = p;
          reason = "explore_frontier";
        }
      }
    }
    if (goal.first < 0 && stage != 1) {
      double best = std::numeric_limits<double>::infinity();
      for (const auto &row : paths.cost) {
        Point p = row.first;
        if (paths.first.at(p).empty() || w.visits.count(p))
          continue;
        double score =
            distance(p, {f.width / 2, f.height / 2}) + row.second * .02;
        if (score < best) {
          best = score;
          goal = p;
          reason = "patrol";
        }
      }
    }
  }
  if (goal.first >= 0 && paths.first.count(goal) &&
      !paths.first.at(goal).empty()) {
    if (stage != 1 && candidates.empty()) {
      auto t = assess("move", paths.first.at(goal));
      if (t.utility + .05 <= tactical_utility) {
        reason = tactical_reason;
        return tactical_action;
      }
    }
    if (stage != 1 &&
        assess("move", paths.first.at(goal)).pepper_bonus > 0)
      reason = "pepper_kill_opportunity";
    return move(f, paths.first.at(goal), events);
  }
  if (stage != 1 && tactical_utility > waiting.utility) {
    reason = tactical_reason;
    return tactical_action;
  }
  return action(f, "wait", "", stage);
}
} // namespace rmtc
