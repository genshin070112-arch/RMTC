/*
 * 文件：include/rmtc_bot/protocol.hpp
 * 作用：通信协议与 JSON 基础（接口声明；具体行为见对应 .cpp）
 * 将感知 JSON 校验为 Frame，将相对坐标转为绝对坐标，并构造合法动作 JSON。
 * 输入裁判感知/字段；输出 Frame、坐标或动作。由 node/controller/planner 使用。
 * 配套文件：src/protocol.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace rmtc {
using Json = nlohmann::json;
using Point = std::pair<int, int>;
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
Json strict_json(const std::string &, size_t limit = 2097152);
int integer(const Json &, const std::string &, int low = 0,
            int high = 10000000);
double number(const Json &, const std::string &, double low = 0,
              double high = 10000000);
Point point(const Json &, int width, int height);
Point offset(const std::string &);
struct Entity {
  std::string type, id, team;
  Point xy;
  double value = 0, hp = 0;
  bool protected_spawn = false, defending = false;
};
struct Frame {
  int tick = 0, width = 0, height = 0, radius = 0;
  std::string session, seat;
  Point position;
  double hp = 0;
  // v1.1：裁判已结算的当前 D，可下降/带小数/超过500；不再自行加奖或扣死亡。
  double effective_damage = 0;
  bool alive = false, protected_spawn = false;
  std::vector<Entity> entities;
  Json missions, score, receipt, raw;
  std::set<std::string> completed;
  std::string event_text;
  std::vector<std::string>
      warnings; // 非关键任务条目隔离诊断，几何/自车仍严格。
};
Frame parse_frame(const std::string &, const std::string &seat);
extern const std::map<std::string, Point> directions;
std::string opposite(const std::string &);
Json action(const Frame &, const std::string &,
            const std::string &direction = "", int stage = 1);
int distance(Point, Point);
std::string attack_direction(Point, Point);
} // namespace rmtc
