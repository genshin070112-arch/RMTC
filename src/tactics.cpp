/*
 * 文件：src/tactics.cpp
 * 作用：用 v1.1 结算顺序比较攻击、防御和成功入点的局部收益。
 * 只读本队感知和模型事件；普通攻击先于移动，辣椒仅在战斗存活并成功入点后预测。
 * 敌人当前动作未知：分别估计维持上帧防御与可见敌人转为攻击本车两种情形，采用较低收益。
 * 敌人后续移动、未知火线及场外状态不作真值；真实分数只采用最新
 * score.effective_damage。
 */
#include "rmtc_bot/tactics.hpp"
#include <algorithm>
#include <cmath>
namespace rmtc {
namespace {
double value(const std::map<std::string, double> &m, const std::string &k) {
  auto i = m.find(k);
  return i == m.end() ? 0 : i->second;
}
TacticalEstimate scenario(const Frame &f, const Events &events,
                          const std::string &action,
                          const std::string &direction, bool keep_defense, bool anticipate) {
  std::vector<Entity> cars;
  Entity me;
  me.team = f.seat;
  me.xy = f.position;
  me.hp = f.hp;
  me.protected_spawn = f.protected_spawn;
  me.defending = action == "defend";
  cars.push_back(me);
  for (auto e : f.entities)
    if (e.type == "robot") {
      e.defending = keep_defense && e.defending;
      cars.push_back(e);
    }
  const auto purity = events.active("purity", f.tick);
  std::map<std::string, double> health;
  for (const auto &e : cars)
    health[e.team] = e.hp;
  std::vector<Hit> hits;
  auto add_attack = [&](const Entity &attacker, const std::string &dir) {
    if (attacker.protected_spawn || attacker.hp <= 0)
      return;
    const auto boosts = events.effect("unrivaled", attacker.team, f.tick);
    const bool boosted = !boosts.empty();
    std::vector<const Entity *> targets;
    for (const auto &target : cars) {
      if (target.team == attacker.team || target.hp <= 0)
        continue;
      if (boosted) {
        // 无双仍采用规则规定的 5×5 方形，方向不限制目标集合。
        if (std::max(std::abs(attacker.xy.first - target.xy.first),
                     std::abs(attacker.xy.second - target.xy.second)) <= 2)
          targets.push_back(&target);
      } else if (attack_direction(attacker.xy, target.xy) == dir)
        targets.push_back(&target);
    }
    if (!boosted && !targets.empty()) {
      auto nearest = *std::min_element(
          targets.begin(), targets.end(), [&](auto a, auto b) {
            return distance(attacker.xy, a->xy) < distance(attacker.xy, b->xy);
          });
      targets = {nearest}; // 保护目标也挡住其后目标；障碍不挡攻击。
    }
    for (auto target : targets) {
      if (target->protected_spawn)
        continue;
      if (!purity.empty() && target->defending) {
        hits.push_back({target->team, attacker.team,
                        purity.back().value("damage", 20.0), false});
        continue;
      }
      double nominal =
          boosted ? boosts.back().value(target->defending ? "defended_damage"
                                                          : "damage",
                                        target->defending ? 10.0 : 50.0)
                  : (target->defending ? 10.0 : 20.0);
      const auto vulnerability =
          events.effect("vulnerable", target->team, f.tick);
      if (!vulnerability.empty())
        nominal *= vulnerability.back().value("multiplier", 2.0);
      hits.push_back({attacker.team, target->team, nominal, true});
    }
  };
  if (action == "attack")
    add_attack(cars.front(), direction);
  for (size_t i = 1; i < cars.size(); ++i) {
    const auto &enemy = cars[i];
    if (enemy.defending)
      continue;
    auto dir = attack_direction(enemy.xy, f.position);
    if (!events.effect("unrivaled", enemy.team, f.tick).empty() &&
        std::max(std::abs(enemy.xy.first - f.position.first),
                 std::abs(enemy.xy.second - f.position.second)) <= 2)
      dir = "up";
    if (!dir.empty())
      add_attack(enemy, dir);
  }
  auto combat = resolve_batch(health, hits);
  TacticalEstimate out;
  out.attack_credit = value(combat.actual_attack, f.seat);
  out.bonuses = value(combat.bonuses, f.seat);
  health = combat.hp;
  // 移动不能躲开本 Tick 已结算的攻击；死亡者不能继续移动/触发辣椒。
  if (action == "move" && value(health, f.seat) > 0 &&
      directions.count(direction)) {
    auto delta = directions.at(direction);
    Point dest = {f.position.first + delta.first,
                  f.position.second + delta.second};
    std::vector<Hit> fire;
    for (const auto &event : events.active("pepper", f.tick)) {
      bool enters = false;
      for (const auto &p : event.at("points"))
        enters |= dest == Point(p[0].get<int>(), p[1].get<int>());
      if (!enters)
        continue;
      for (size_t i = 1; i < cars.size(); ++i) {
        const auto &target = cars[i];
        if (value(health, target.team) > 0 && !target.protected_spawn &&
            target.xy != dest &&
            (target.xy.first == dest.first || target.xy.second == dest.second))
          fire.push_back(
              {f.seat, target.team, event.value("damage", 50.0), false});
      }
    }
    // 同 Tick 所有已预测的火线作为同一批次；战斗中已经死亡者不再次分奖。
    const auto pepper = resolve_batch(health, fire);
    out.pepper_bonus = value(pepper.bonuses, f.seat);
    out.bonuses += out.pepper_bonus;
    health = pepper.hp;
  }
  out.hp = value(health, f.seat);
  out.alive = out.hp > 0;
  out.damage = projected_damage(f.effective_damage, out.attack_credit,
                                out.bonuses, out.alive ? 0 : 1);
  out.utility = damage_utility(f.effective_damage, out.damage) +
                .04 * (out.hp - f.hp) - (out.alive ? 0 : 6);
  // 伤害任务只能在结算时存活才有机会完成；已完成 ID 绝不根据分数下降撤销。
  for (const auto &m : f.missions)
    if (out.alive && m["type"] == "effective_damage" &&
        !f.completed.count(m["id"].get<std::string>()) &&
        f.effective_damage < m.at("target").get<double>() &&
        out.damage >= m.at("target").get<double>())
      out.utility += 5;
  if (anticipate && out.alive) {
    // 一步延续假设：敌人保持当前位置、存活者下Tick可攻击。它是风险估计，
    // 不是对敌人动作的保证。已被本动作击杀的敌人不能继续制造威胁。
    Frame next = f;
    ++next.tick;
    next.hp = out.hp;
    next.effective_damage = out.damage;
    if (action == "move" && directions.count(direction)) {
      auto d = directions.at(direction);
      next.position = {f.position.first+d.first, f.position.second+d.second};
    }
    next.entities.clear();
    for (auto enemy : f.entities) if (enemy.type == "robot" && value(health,enemy.team)>0) {
      enemy.hp=value(health,enemy.team); next.entities.push_back(enemy);
    }
    // 当前战斗存活后才结算回血；新入天使点与已有增益取较大速率，不叠层。
    double heal=0;
    for (const auto &e : events.effect("angel",f.seat,f.tick))
      heal=std::max(heal,e.value("heal_per_tick",10.0));
    if (action=="move") for (const auto &e : events.active("angel",f.tick))
      if (f.tick < e.at("start_tick").get<int>()+e.value("acquire_ticks",20))
        for (const auto &point : e.at("points"))
          if (next.position==Point(point[0].get<int>(),point[1].get<int>()))
            heal=std::max(heal,e.value("heal_per_tick",10.0));
    next.hp=std::min(100.0,next.hp+heal);
    const auto exposed=scenario(next,events,"wait","",false,false);
    const double loss=next.hp-exposed.hp;
    // 沿用现有HP/死亡偏好；连续风险按血量余裕平滑缩放，不增加地图专用阈值。
    out.future_risk=.04*loss*std::min(1.0,loss/next.hp);
    if (!exposed.alive)
      out.future_risk += 6 + std::max(0.0,combat_score(out.damage)-
          combat_score(projected_damage(out.damage,0,0,1)));
    out.utility-=out.future_risk;
  }
  return out;
}
} // namespace
TacticalEstimate assess_tactic(const Frame &f, const Events &events,
                               const std::string &action,
                               const std::string &direction, bool anticipate) {
  // 普通移动本Tick已必死时，延续风险不能让“本Tick能活的防御”反而输给立刻死亡。
  // 此时先比较即时自救；有移动余地时才比较下一Tick的火线暴露。
  // 前瞻仅用于低血量自救（40为两次普通20伤害的血量），不把健康期
  // 下一Tick假设WAIT的损失扣到当前可兑现攻击上。仍需当前移动可存活。
  anticipate = anticipate && f.hp <= 40 &&
               scenario(f, events, "wait", "", false, false).alive;
  auto a = scenario(f, events, action, direction, true, anticipate);
  auto b = scenario(f, events, action, direction, false, anticipate);
  // defending 仅是最近状态。比较两种假设，不把上一帧防御当作下一帧的保证。
  return a.utility < b.utility ? a : b;
}
bool threatens(const Entity &enemy, Point target, const Events &events, int tick) {
  if (enemy.protected_spawn || enemy.hp<=0) return false;
  if (!events.effect("unrivaled",enemy.team,tick).empty())
    return std::max(std::abs(enemy.xy.first-target.first),
                    std::abs(enemy.xy.second-target.second))<=2;
  return !attack_direction(enemy.xy,target).empty();
}
bool needs_damage(const Frame &f) {
  for (const auto &m : f.missions)
    if (m["type"] == "effective_damage" &&
        !f.completed.count(m["id"].get<std::string>()) &&
        f.effective_damage < m.at("target").get<double>())
      return true;
  return false;
}
} // namespace rmtc
