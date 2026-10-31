/*
 * 文件：src/protocol.cpp
 * 作用：通信协议与 JSON 基础（实现文件）
 * 将感知 JSON 校验为 Frame，将相对坐标转为绝对坐标，并构造合法动作 JSON。
 * 输入裁判感知/字段；输出 Frame、坐标或动作。由 node/controller/planner 使用。
 * 配套文件：include/rmtc_bot/protocol.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/protocol.hpp"
#include <cmath>
#include <limits>
namespace rmtc {
const std::map<std::string, Point> directions = {
    {"up", {0, -1}}, {"down", {0, 1}}, {"left", {-1, 0}}, {"right", {1, 0}}};
Json strict_json(const std::string &text, size_t limit) {
  if (text.size() > limit)
    throw Error("JSON too large");
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
    if (depth > 64)
      throw Error("JSON nesting too deep");
    if (event == Json::parse_event_t::object_start)
      keys.emplace_back();
    if (event == Json::parse_event_t::key &&
        !keys.back().insert(value.get<std::string>()).second)
      throw Error("duplicate JSON key");
    if (event == Json::parse_event_t::object_end)
      keys.pop_back();
    return true;
  };
  try {
    return Json::parse(text, callback);
  } catch (const std::exception &) {
    throw Error("invalid JSON");
  }
}
int integer(const Json &v, const std::string &field, int low, int high) {
  if (!v.is_number_integer())
    throw Error(field + ": expected integer");
  long long n;
  try {
    n = v.get<long long>();
  } catch (...) {
    throw Error(field + ": integer overflow");
  }
  if (n < low || n > high)
    throw Error(field + ": integer range");
  return static_cast<int>(n);
}
double number(const Json &v, const std::string &field, double low,
              double high) {
  if (!v.is_number())
    throw Error(field + ": expected number");
  double n = v.get<double>();
  if (!std::isfinite(n) || n < low || n > high)
    throw Error(field + ": number range");
  return n;
}
Point point(const Json &v, int w, int h) {
  if (!v.is_array() || v.size() != 2)
    throw Error("coordinate must be [x,y]");
  return {integer(v[0], "x", 0, w - 1), integer(v[1], "y", 0, h - 1)};
}
Point offset(const std::string &s) {
  if (s.size() != 89)
    throw Error("position must have 89 characters");
  int ones = 0, twos = 0;
  Point d = {0, 0};
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      char ch = s[r * 10 + c];
      if (ch == '1') {
        ++ones;
        if (r != 4 || c != 4)
          throw Error("center marker invalid");
      } else if (ch == '2') {
        ++twos;
        d = {c - 4, r - 4};
      } else if (ch != '0')
        throw Error("position character invalid");
    }
    if (r < 8 && s[r * 10 + 9] != '\n')
      throw Error("position newline invalid");
  }
  if (ones != 1 || twos > 1)
    throw Error("position markers invalid");
  return d;
}
static bool boolean(const Json &v, const std::string &n) {
  if (!v.is_boolean())
    throw Error(n + ": expected boolean");
  return v.get<bool>();
}
Frame parse_frame(const std::string &text, const std::string &seat) {
  try {
    Json d = strict_json(text);
    if (!d.is_object())
      throw Error("perception must be object");
    Frame f;
    f.raw = d;
    f.tick = integer(d.at("tick"), "tick");
    f.session = d.at("session_id").get<std::string>();
    if (f.session.empty() || f.session.size() > 256)
      throw Error("invalid session_id");
    auto me = d.at("self"), m = d.at("map");
    f.seat = me.at("team").get<std::string>();
    if (f.seat != seat)
      throw Error("wrong seat");
    f.width = integer(m.at("width"), "width", 1, 256);
    f.height = integer(m.at("height"), "height", 1, 256);
    f.radius = integer(d.at("vision_radius"), "radius", 0, 4);
    f.position = point(me.at("position"), f.width, f.height);
    f.hp = number(me.at("hp"), "hp");
    f.alive = boolean(me.at("alive"), "alive");
    f.protected_spawn = boolean(me.at("protected"), "protected");
    boolean(me.at("defending"), "defending");
    const auto &entities = d.at("visible_entities");
    if (!entities.is_array() || entities.size() > 4096)
      throw Error("invalid entities");
    for (const auto &e : entities) {
      Entity v;
      v.type = e.at("type").get<std::string>();
      if (v.type != "robot" && v.type != "obstacle" &&
          v.type != "resource_normal" && v.type != "resource_high")
        continue;
      auto rel = offset(e.at("position").get<std::string>());
      if (std::max(std::abs(rel.first), std::abs(rel.second)) > f.radius)
        throw Error("outside vision");
      v.xy = point(Json::array({f.position.first + rel.first,
                                f.position.second + rel.second}),
                   f.width, f.height);
      if (v.type == "robot") {
        v.team = e.at("team").get<std::string>();
        if (v.team.size() != 1 ||
            std::string("ABCD").find(v.team) == std::string::npos ||
            v.team == seat)
          throw Error("invalid enemy team");
        v.hp = number(e.at("hp"), "enemy.hp");
        v.protected_spawn = boolean(e.at("protected"), "enemy.protected");
        v.defending = boolean(e.at("defending"), "enemy.defending");
      } else if (v.type != "obstacle") {
        v.id = e.at("id").get<std::string>();
        v.value = number(e.at("value"), "value", 0.00001);
      }
      f.entities.push_back(v);
    }
    auto missions = d.value("missions", Json::array());
    if (!missions.is_array() || missions.size() > 64)
      throw Error("invalid missions");
    f.missions = Json::array();
    std::set<std::string> mission_ids;
    for (const auto &m : missions) {
      try {
        auto id = m.at("id").get<std::string>();
        auto type = m.at("type").get<std::string>();
        if (id.empty() || !mission_ids.insert(id).second)
          throw Error("invalid mission id");
        if (type == "reach")
          point(m.at("position"), f.width, f.height);
        else if (type == "resource_value" || type == "effective_damage" ||
                 type == "normal_resources" || type == "high_resources")
          number(m.at("target"), "target", 0);
        else
          throw Error("unsupported mission type");
        f.missions.push_back(m);
      } catch (const std::exception &) {
        // 缺少目标不影响可靠自车/地图；隔离这项任务，基础采集继续。
        f.warnings.push_back("mission_" + std::to_string(f.warnings.size()) +
                             "_ignored");
      }
    }
    f.raw["missions"] = f.missions;
    f.score = d.at("score");
    if (!f.score.is_object())
      throw Error("invalid score");
    for (auto name : {"resource_value", "mission_completed", "invalid_actions"})
      number(f.score.value(name, Json(0)), name);
    // v1.1 的 D 可能下降，且不在500或旧通用字段上限处截断；保持本帧值。
    f.effective_damage =
        number(f.score.at("effective_damage"), "effective_damage", 0,
               std::numeric_limits<double>::max());
    auto completed = f.score.value("mission_completed_ids", Json::array());
    if (!completed.is_array())
      throw Error("invalid completed ids");
    for (const auto &id : completed)
      f.completed.insert(id.get<std::string>());
    f.receipt = me.value("last_action", Json());
    if (!f.receipt.is_null()) {
      integer(f.receipt.at("tick"), "receipt.tick");
      auto a = f.receipt.at("action").get<std::string>();
      if (a != "move" && a != "attack" && a != "defend" && a != "wait")
        throw Error("invalid receipt action");
      if (f.receipt.contains("moved"))
        boolean(f.receipt["moved"], "moved");
    }
    f.event_text = d.value("event_text", std::string());
    return f;
  } catch (const Error &) {
    throw;
  } catch (const std::exception &) {
    throw Error("perception schema invalid");
  }
}
std::string opposite(const std::string &d) {
  if (d == "up")
    return "down";
  if (d == "down")
    return "up";
  if (d == "left")
    return "right";
  if (d == "right")
    return "left";
  return "";
}
Json action(const Frame &f, const std::string &requested,
            const std::string &dir, int stage) {
  std::string a = requested;
  if (a != "move" && a != "attack" && a != "defend" && a != "wait")
    a = "wait";
  if (!f.alive || (stage == 1 && a != "move" && a != "wait") ||
      (f.protected_spawn && a == "attack"))
    a = "wait";
  if ((a == "move" || a == "attack") && !directions.count(dir))
    a = "wait";
  Json out = {{"tick", f.tick}, {"session_id", f.session}, {"action", a}};
  if (a == "move" || a == "attack")
    out["direction"] = dir;
  if (out.dump().size() > 4096)
    throw Error("action too large");
  return out;
}
int distance(Point a, Point b) {
  return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}
std::string attack_direction(Point a, Point b) {
  int dx = b.first - a.first, dy = b.second - a.second;
  if (dy == 0 && std::abs(dx) >= 1 && std::abs(dx) <= 2)
    return dx > 0 ? "right" : "left";
  if (dx == 0 && std::abs(dy) >= 1 && std::abs(dy) <= 2)
    return dy > 0 ? "down" : "up";
  return "";
}
} // namespace rmtc
