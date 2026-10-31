/*
 * 文件：include/rmtc_bot/tactics.hpp
 * 作用：v1.1 可见范围内的战斗、反伤防御与主动踩辣椒收益评估接口。
 * 输入当前感知、经模型校验的事件和候选动作；输出预测分数/存活/血量/收益。
 * 配套：src/tactics.cpp。预测敌人动作有不确定性，结果不写回 Frame/World
 * 或真实积分。
 */
#pragma once
#include "events.hpp"
#include "scoring.hpp"
namespace rmtc {
struct TacticalEstimate {
  double utility = 0, damage = 0, hp = 0, future_risk = 0;
  double attack_credit = 0, bonuses = 0, pepper_bonus = 0;
  bool alive = true;
};
TacticalEstimate assess_tactic(const Frame &, const Events &,
                               const std::string &action,
                               const std::string &direction = "",
                               bool anticipate = false);
// 只判断已知敌人的攻击几何；不把未来敌人移动当作已知事实。
bool threatens(const Entity &, Point, const Events &, int tick);
bool needs_damage(const Frame &);
} // namespace rmtc
