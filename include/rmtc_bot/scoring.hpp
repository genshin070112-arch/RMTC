/*
 * 文件：include/rmtc_bot/scoring.hpp
 * 作用：v1.1 计分规则与单批次伤害预测接口。
 * 输入假设的血量、伤害来源及本 Tick
 * 死亡次数；输出比例贡献、去重分奖和预测有效伤害。 配套：src/scoring.cpp；由
 * tactics 使用。这里只做规划预测，不修改裁判感知中的真实分数。
 */
#pragma once
#include "protocol.hpp"
namespace rmtc {
constexpr double combat_full_damage = 500.0;
constexpr double kill_reward = 60.0;
constexpr double death_deduction = 60.0;
struct Hit {
  std::string source, target;
  double nominal = 0;
  bool attack_credit = true; // 反伤、辣椒为 false；仍可作为击杀分奖贡献者。
};
struct DamageBatch {
  std::map<std::string, double> hp, actual_attack, bonuses;
  std::set<std::string> killed;
};
DamageBatch resolve_batch(const std::map<std::string, double> &hp,
                          const std::vector<Hit> &hits);
double projected_damage(double previous, double attacks, double bonuses,
                        int deaths);
double combat_score(double damage);
double damage_utility(double previous, double next);
} // namespace rmtc
