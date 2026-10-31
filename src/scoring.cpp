/*
 * 文件：src/scoring.cpp
 * 作用：v1.1 的 500 点门槛、60 点分奖/死亡扣除与比例伤害的纯函数预测。
 * 单次调用仅代表一个同时结算批次；每批重新建立贡献者集合，死亡目标不重复分奖。
 * 反伤/辣椒命中不计普通攻击贡献；环境死亡由调用方另计死亡次数，不伪造来源。
 * 输入来自 tactics 的合法可见信息假设；实际 D 始终直接读取 protocol
 * 的当前感知。
 */
#include "rmtc_bot/scoring.hpp"
#include <algorithm>
#include <cmath>
namespace rmtc {
DamageBatch resolve_batch(const std::map<std::string, double> &hp,
                          const std::vector<Hit> &hits) {
  DamageBatch out;
  out.hp = hp;
  for (const auto &[target, health] : hp) {
    if (!std::isfinite(health) || health < 0)
      throw Error("invalid predicted hp");
    if (health == 0)
      continue;
    double total = 0;
    for (const auto &h : hits) {
      if (!std::isfinite(h.nominal) || h.nominal < 0)
        throw Error("invalid predicted hit");
      if (h.target == target && hp.count(h.source) && hp.at(h.source) > 0 &&
          h.source != target)
        total += h.nominal;
    }
    if (total <= 0)
      continue;
    const double loss = std::min(health, total);
    out.hp[target] = health - loss;
    std::set<std::string> contributors;
    for (const auto &h : hits) {
      if (h.target != target || h.nominal <= 0 || !hp.count(h.source) ||
          hp.at(h.source) <= 0 || h.source == target)
        continue;
      const double actual = loss * (h.nominal / total);
      if (actual > 0) {
        contributors.insert(h.source);
        if (h.attack_credit)
          out.actual_attack[h.source] += actual;
      }
    }
    if (out.hp[target] == 0) {
      out.killed.insert(target);
      for (const auto &source : contributors)
        out.bonuses[source] += kill_reward / contributors.size();
    }
  }
  return out;
}
double projected_damage(double previous, double attacks, double bonuses,
                        int deaths) {
  // 先加本 Tick 全部攻击/分奖，再统一扣死亡；不能在每个子阶段各自钳零。
  return std::max(0.0, previous + attacks + bonuses - deaths * death_deduction);
}
double combat_score(double damage) {
  return 15.0 * std::min(std::max(damage, 0.0) / combat_full_damage, 1.0);
}
double damage_utility(double previous, double next) {
  // 满分以外仍保留小幅排序收益；未满分时，直接采用可增加的比赛分（每点D约0.03分），供全部动作共同比较。
  return combat_score(next) - combat_score(previous) + .001 * (next - previous);
}
} // namespace rmtc
