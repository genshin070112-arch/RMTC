/*
 * 文件：src/events.cpp
 * 作用：事件与效果管理（实现文件）
 * 校验模型输出的结构化事件、效果和目标，管理有效期，并根据合法历史推断部分效果。
 * 输入模型 JSON、感知与本队历史；输出危险区域、效果状态等供 controller/planner
 * 使用。
 * 配套文件：include/rmtc_bot/events.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/events.hpp"
#include <algorithm>
#include <cmath>
namespace rmtc {
namespace {
int inference_expiry(const Json &e) {
  const int start = e.at("start_tick"), end = e.at("end_tick");
  int expiry = std::max(end, start + e.value("effect_ticks", 0));
  // 天使点消失后，最后一次合法入点的回血仍可持续。模型回包迟到时，
  // 保留这段公告以便用本队成功移动历史恢复效果；不会重新激活点位。
  if (e.at("kind") == "angel") {
    const int acquire_end =
        std::min(end, start + e.value("acquire_ticks", end - start));
    expiry = std::max(end, acquire_end - 1 + e.value("effect_ticks", 10));
  }
  return expiry;
}
} // namespace
static Json validate_model(std::string s, const std::string &session, int w,
                           int h) {
  try {
    if (s.rfind("```json\n", 0) == 0 && s.size() > 11 &&
        s.substr(s.size() - 3) == "```")
      s = s.substr(8, s.size() - 11);
    auto d = strict_json(s, 131072);
    if (!d.is_object() || d.at("session_id") != session)
      throw Error("model session mismatch");
    Json out = {{"session_id", session},
                {"events", Json::array()},
                {"effects", Json::array()},
                {"goal", nullptr}};
    const std::set<std::string> kinds = {"explosion", "illusion",    "angel",
                                         "unrivaled", "world_split", "pepper",
                                         "purity",    "pan"};
    auto list = d.value("events", Json::array());
    if (!list.is_array() || list.size() > 128)
      throw Error("invalid model events");
    for (const auto &raw : list) {
      auto kind = raw.at("kind").get<std::string>();
      int start = integer(raw.at("start_tick"), "event.start"),
          end = integer(raw.at("end_tick"), "event.end");
      if (!kinds.count(kind) || end <= start || end - start > 10000)
        throw Error("invalid event lifetime/kind");
      Json e = {{"kind", kind}, {"start_tick", start}, {"end_tick", end}};
      if (kind == "explosion" || kind == "illusion") {
        auto r = raw.at("rect");
        if (!r.is_array() || r.size() != 4)
          throw Error("invalid rect");
        auto a = point(Json::array({r[0], r[1]}), w, h),
             b = point(Json::array({r[2], r[3]}), w, h);
        if (a.first > b.first || a.second > b.second)
          throw Error("reversed rect");
        e["rect"] = r;
      }
      if (kind == "angel" || kind == "pepper") {
        auto pts = raw.at("points");
        if (!pts.is_array() || pts.empty() || pts.size() > 128)
          throw Error("invalid points");
        for (const auto &p : pts)
          point(p, w, h);
        e["points"] = pts;
      }
      if (kind == "unrivaled") {
        point(raw.at("center"), w, h);
        e["center"] = raw["center"];
      }
      if (kind == "world_split") {
        e["row"] = integer(raw.at("row"), "row", 0, h - 1);
        e["col"] = integer(raw.at("col"), "col", 0, w - 1);
      }
      for (auto key :
           {"damage", "defended_damage", "heal_per_tick", "multiplier"})
        if (raw.contains(key))
          e[key] = number(raw[key], key, 0, 10000);
      for (auto key : {"effect_ticks", "acquire_ticks", "movement_window_ticks",
                       "stay_ticks", "size", "attack_radius"})
        if (raw.contains(key))
          e[key] = integer(raw[key], key, 1, 10000);
      if (kind == "illusion" && !e.contains("effect_ticks"))
        throw Error("missing illusion duration");
      out["events"].push_back(e);
    }
    list = d.value("effects", Json::array());
    if (!list.is_array() || list.size() > 128)
      throw Error("invalid effects");
    for (const auto &raw : list) {
      auto kind = raw.at("kind").get<std::string>(),
           team = raw.at("team").get<std::string>();
      const std::set<std::string> effects = {"illusion", "angel", "unrivaled",
                                             "vulnerable"};
      int start = integer(raw.at("start_tick"), "effect.start"),
          end = integer(raw.at("end_tick"), "effect.end");
      if (!effects.count(kind) || team.size() != 1 ||
          std::string("ABCD").find(team) == std::string::npos || end <= start ||
          end - start > 10000)
        throw Error("invalid effect");
      Json e = {{"kind", kind},
                {"team", team},
                {"start_tick", start},
                {"end_tick", end}};
      for (auto key :
           {"damage", "defended_damage", "multiplier", "heal_per_tick"})
        if (raw.contains(key))
          e[key] = number(raw[key], key, 0, 10000);
      out["effects"].push_back(e);
    }
    if (d.contains("goal") && !d["goal"].is_null()) {
      point(d["goal"], w, h);
      out["goal"] = d["goal"];
    }
    return out;
  } catch (const Error &) {
    throw;
  } catch (const std::exception &) {
    throw Error("model output schema invalid");
  }
}
// 模型格式容错只修复无歧义表示；绝不从自然语言/残缺 JSON 猜事件。
Json model_output(std::string s, const std::string &session, int w, int h) {
  auto trim = [](std::string x) {
    auto a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
  };
  s = trim(s);
  if (s.rfind("```", 0) == 0 && s.size() >= 6 &&
      s.substr(s.size() - 3) == "```") {
    auto line = s.find('\n');
    auto tag = line == std::string::npos ? std::string("invalid")
                                         : trim(s.substr(3, line - 3));
    if (tag.empty() || tag == "json" || tag == "JSON")
      s = trim(s.substr(line + 1, s.size() - line - 4));
  }
  Json d = strict_json(s, 131072);
  if (!d.is_object() || d.value("session_id", std::string()) != session ||
      !d.contains("events") || !d["events"].is_array() ||
      d["events"].size() > 128 || !d.contains("effects") ||
      !d["effects"].is_array() || d["effects"].size() > 128)
    throw Error("model envelope/session invalid");
  auto normalize = [&](auto &&self, Json &v) -> void {
    if (v.is_object() || v.is_array()) {
      for (auto &x : v)
        self(self, x);
    } else if (v.is_number_float()) {
      double x = v.get<double>();
      if (std::isfinite(x) && x >= 0 && x <= 10000000 && std::floor(x) == x)
        v = static_cast<int>(x);
    }
  };
  normalize(normalize, d);
  Json out = {{"session_id", session},
              {"events", Json::array()},
              {"effects", Json::array()},
              {"goal", nullptr},
              {"validation_warnings", Json::array()}};
  size_t valid = 0;
  for (auto key : {"events", "effects"}) {
    size_t i = 0;
    for (const auto &entry : d[key]) {
      Json one = {{"session_id", session},
                  {"events", Json::array()},
                  {"effects", Json::array()},
                  {"goal", nullptr}};
      one[key].push_back(entry);
      try {
        auto checked = validate_model(one.dump(), session, w, h);
        auto item = checked[key][0];
        // v1.1爆炸为激活Tick的一次判定，不能按模型给出的长窗口重复伤害。
        if (std::string(key) == "events" && item["kind"] == "explosion")
          item["end_tick"] = item["start_tick"].get<int>() + 1;
        out[key].push_back(item);
        ++valid;
      } catch (const std::exception &) {
        out["validation_warnings"].push_back(std::string(key) + "[" +
                                             std::to_string(i) + "] invalid");
      }
      ++i;
    }
  }
  if (d.contains("goal") && !d["goal"].is_null()) {
    try {
      point(d["goal"], w, h);
      out["goal"] = d["goal"];
      ++valid;
    } catch (const std::exception &) {
      out["validation_warnings"].push_back("goal invalid");
    }
  }
  if (!out["validation_warnings"].empty() && !valid)
    throw Error("model contains no valid items");
  return out;
}
bool inside(Point p, const Json &r) {
  return p.first >= r[0].get<int>() && p.second >= r[1].get<int>() &&
         p.first <= r[2].get<int>() && p.second <= r[3].get<int>();
}
void Events::ingest(const Json &d, int now) {
  for (const auto &e : d["events"]) {
    int start = e["start_tick"], expiry = inference_expiry(e);
    if (expiry > now)
      events[{e["kind"].get<std::string>(), start}] = e;
  }
  for (const auto &e : d["effects"])
    if (e["end_tick"].get<int>() > now)
      effects[{e["kind"].get<std::string>(), e["team"].get<std::string>(),
               e["start_tick"].get<int>()}] = e;
  if (!d["goal"].is_null()) {
    goal = {d["goal"][0].get<int>(), d["goal"][1].get<int>()};
    goal_until = now + 12;
  }
}
void Events::expire(int tick) {
  for (auto it = events.begin(); it != events.end();) {
    if (inference_expiry(it->second) <= tick)
      it = events.erase(it);
    else
      ++it;
  }
  for (auto it = effects.begin(); it != effects.end();)
    if (it->second["end_tick"].get<int>() <= tick)
      it = effects.erase(it);
    else
      ++it;
  if (tick >= goal_until)
    goal = {-1, -1};
}
std::vector<Json> Events::active(const std::string &kind, int tick) const {
  std::vector<Json> out;
  for (const auto &row : events) {
    const auto &e = row.second;
    if (e["kind"] == kind && e["start_tick"].get<int>() <= tick &&
        tick < e["end_tick"].get<int>())
      out.push_back(e);
  }
  return out;
}
std::vector<Json> Events::effect(const std::string &kind,
                                 const std::string &team, int tick) const {
  std::vector<Json> out;
  for (const auto &row : effects) {
    const auto &e = row.second;
    if (e["kind"] == kind && e["team"] == team &&
        e["start_tick"].get<int>() <= tick && tick < e["end_tick"].get<int>())
      out.push_back(e);
  }
  return out;
}
void Events::infer_illusion(
    const std::map<int, std::pair<Point, bool>> &history,
    const std::string &team, int tick) {
  for (const auto &row : events) {
    const auto &e = row.second;
    if (e["kind"] != "illusion")
      continue;
    int start = e["start_tick"], end = start + e["effect_ticks"].get<int>();
    auto h = history.find(start);
    if (start <= tick && tick < end && h != history.end() && h->second.second &&
        inside(h->second.first, e["rect"]))
      effects[{"illusion", team, start}] = {{"kind", "illusion"},
                                            {"team", team},
                                            {"start_tick", start},
                                            {"end_tick", end}};
  }
}
bool Events::blocked(Point p, int tick, Point spawn) const {
  for (const auto &e : active("world_split", tick))
    if (p != spawn && (p.first == e["col"] || p.second == e["row"]))
      return true;
  return false;
}
double Events::danger(Point p, int tick, bool protection) const {
  double risk = 0;
  for (const auto &row : events) {
    const auto &e = row.second;
    int start = e["start_tick"], end = e["end_tick"];
    if (e["kind"] == "explosion" && start <= tick && tick < end &&
        inside(p, e["rect"]) && !protection)
      risk += e.value("damage", 80.0);
    if (e["kind"] == "world_split" && start == tick &&
        (p.first == e["col"] || p.second == e["row"]))
      risk += 200;
  }
  return risk;
}
double Events::entry_penalty(Point p, int tick) const {
  // 辣椒交点免疫自己的该条火线，不能把进入触发点一律当作自身损血。
  // 未知的他车入点也不能推断为已发生。真实可见的主动入点评估见 tactics.cpp。
  (void)p;
  (void)tick;
  return 0;
}

void Events::infer_gains(const std::map<int, std::pair<Point, bool>> &history,
                         const std::map<int, Point> &moves,
                         const std::string &team, int tick) {
  for (const auto &row : events) {
    const auto &e = row.second;
    std::string kind = e.at("kind");
    int start = e.at("start_tick"), end = e.at("end_tick");
    if (kind == "angel" || kind == "pan") {
      int acquire_end = std::min(
          end, start + e.value(kind == "angel" ? "acquire_ticks"
                                               : "movement_window_ticks",
                               kind == "pan" ? 20 : end - start));
      for (const auto &movement : moves) {
        int t = movement.first;
        if (t < start || t >= acquire_end || t >= tick)
          continue;
        bool entered = kind == "pan";
        if (kind == "angel")
          for (const auto &p : e.at("points"))
            if (movement.second == Point(p[0].get<int>(), p[1].get<int>()))
              entered = true;
        if (!entered)
          continue;
        int expiry = kind == "pan" ? end : t + e.value("effect_ticks", 10);
        if (expiry <= tick)
          continue;
        std::string effect_kind = kind == "pan" ? "vulnerable" : "angel";
        Json effect = {{"kind", effect_kind},
                       {"team", team},
                       {"start_tick", kind == "pan" ? t + 1 : t},
                       {"end_tick", expiry}};
        if (kind == "pan")
          effect["multiplier"] = e.value("multiplier", 2.0);
        else
          effect["heal_per_tick"] = e.value("heal_per_tick", 10.0);
        effects[{effect_kind, team, kind == "pan" ? t + 1 : t}] = effect;
      }
    }
    if (kind == "unrivaled" && start < tick && tick < end) {
      Point center = point(e.at("center"), 10000000, 10000000);
      int half = e.value("size", 3) / 2, streak = 0, granted = -1;
      for (int t = start; t < std::min(tick, end); ++t) {
        auto h = history.find(t + 1);
        if (h != history.end() && h->second.second &&
            std::abs(h->second.first.first - center.first) <= half &&
            std::abs(h->second.first.second - center.second) <= half)
          ++streak;
        else
          streak = 0;
        if (streak >= e.value("stay_ticks", 10)) {
          granted = t + 1;
          break;
        }
      }
      if (granted >= 0)
        effects[{"unrivaled", team, start}] = {
            {"kind", "unrivaled"},
            {"team", team},
            {"start_tick", granted},
            {"end_tick", end},
            {"damage", e.value("damage", 50.0)},
            {"defended_damage", e.value("defended_damage", 10.0)}};
    }
  }
}
} // namespace rmtc
