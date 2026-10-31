/*
 * 文件：src/config.cpp
 * 作用：启动配置与令牌读取（实现文件）
 * 读取命令行、白名单环境配置、令牌文件和 decision_config.json；检查配置合法性。
 * 输入启动参数/本地配置；输出 Config。由 node 使用；.env 作为数据读取，不当作
 * shell 执行。
 * 配套文件：include/rmtc_bot/config.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/config.hpp"
#include "rmtc_bot/planner.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
namespace rmtc {
namespace {
std::string env(const std::string &k, const std::string &fallback = "") {
  auto p = std::getenv(k.c_str());
  return p ? std::string(p) : fallback;
}
int parse_int(const std::string &s) {
  try {
    size_t p = 0;
    int n = std::stoi(s, &p);
    if (p != s.size())
      throw Error("trailing integer data");
    return n;
  } catch (...) {
    throw Error("invalid numeric option");
  }
}
std::string trim(std::string s) {
  auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
const std::set<std::string> allowed = {
    "RMTC_SEAT",       "RMTC_TEAM_NAME",      "RMTC_STAGE",
    "RMTC_MAX_TICKS",  "RMTC_GATEWAY",        "RMTC_MODEL",
    "RMTC_TEAM_TOKEN", "RMTC_TOKEN_FILE",     "RMTC_DECISION_FILE",
    "RMTC_LOG_DIR",    "RMTC_LLM_TIMEOUT_MS", "RMTC_LLM_BUDGET",
    "RMTC_MAX_TOKENS"};
void load_env(const std::string &path) {
  std::istringstream in(read_file(path, 16384));
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    auto p = line.find('=');
    if (p == std::string::npos)
      throw Error("invalid env line");
    auto k = trim(line.substr(0, p)), v = trim(line.substr(p + 1));
    if (!allowed.count(k))
      throw Error("unsupported env key");
    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') &&
        v.back() == v.front())
      v = v.substr(1, v.size() - 2);
    if (!std::getenv(k.c_str()))
      setenv(k.c_str(), v.c_str(), 0);
  }
}
} // namespace
std::string read_file(const std::string &path, size_t limit) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw Error("cannot open configured file");
  std::string out;
  out.resize(limit + 1);
  f.read(out.data(), static_cast<std::streamsize>(out.size()));
  out.resize(static_cast<size_t>(f.gcount()));
  if (out.size() > limit)
    throw Error("configured file too large");
  return out;
}
std::string read_token(const std::string &path) {
  try {
    return trim(path.empty() ? env("RMTC_TEAM_TOKEN") : read_file(path, 4096));
  } catch (...) {
    return "";
  }
}
std::string read_decision(const std::string &path) {
  auto d = strict_json(read_file(path, 4096));
  if (!d.is_object() || d.size() != 1 || !d.contains("decision_type") ||
      !d["decision_type"].is_string())
    throw Error("decision_config must contain only decision_type");
  auto value = d["decision_type"].get<std::string>();
  if (!strategies.count(value))
    throw Error("unregistered decision_type");
  return value;
}
Config config(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--env-file") {
      if (++i >= argc)
        throw Error("env path missing");
      load_env(argv[i]);
    }
  Config c;
  c.seat = env("RMTC_SEAT", "A");
  c.name = env("RMTC_TEAM_NAME", "RMTC-CPP");
  c.stage = parse_int(env("RMTC_STAGE", "-1"));
  c.max_ticks = parse_int(env("RMTC_MAX_TICKS", "0"));
  c.gateway = env("RMTC_GATEWAY", c.gateway);
  c.model = env("RMTC_MODEL");
  c.token_file = env("RMTC_TOKEN_FILE");
  c.decision_file = env("RMTC_DECISION_FILE", c.decision_file);
  c.log_dir = env("RMTC_LOG_DIR", c.log_dir);
  c.timeout_ms = parse_int(env("RMTC_LLM_TIMEOUT_MS", "4000"));
  c.budget = parse_int(env("RMTC_LLM_BUDGET", "48"));
  c.max_tokens = parse_int(env("RMTC_MAX_TOKENS", "1600"));
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "--ros-args")
      break;
    if (key == "--help")
      throw Error("Options: --seat A --name NAME --stage 1|2 --max-ticks N "
                  "--gateway URL --token-file FILE --env-file FILE "
                  "--decision-file FILE --log-dir DIR --model NAME "
                  "--llm-timeout-ms N --llm-budget N --max-tokens N");
    if (++i >= argc)
      throw Error("option value missing");
    std::string value = argv[i];
    if (key == "--env-file")
      continue;
    else if (key == "--seat")
      c.seat = value;
    else if (key == "--name")
      c.name = value;
    else if (key == "--stage")
      c.stage = parse_int(value);
    else if (key == "--max-ticks")
      c.max_ticks = parse_int(value);
    else if (key == "--gateway")
      c.gateway = value;
    else if (key == "--model")
      c.model = value;
    else if (key == "--token-file")
      c.token_file = value;
    else if (key == "--decision-file")
      c.decision_file = value;
    else if (key == "--log-dir")
      c.log_dir = value;
    else if (key == "--llm-timeout-ms")
      c.timeout_ms = parse_int(value);
    else if (key == "--llm-budget")
      c.budget = parse_int(value);
    else if (key == "--max-tokens")
      c.max_tokens = parse_int(value);
    else
      throw Error("unknown option");
  }
  if (c.seat.size() != 1 ||
      std::string("ABCD").find(c.seat) == std::string::npos || c.stage < 0 ||
      c.stage > 2 || (c.stage == 1 && c.seat != "A"))
    throw Error("explicit --stage 1|2 (or RMTC_STAGE) and valid seat required; "
                "put options before --ros-args");
  if (c.name.empty() || c.name.size() > 96 ||
      c.name.find_first_of("\r\n") != std::string::npos)
    throw Error("invalid team name");
  if (!c.max_ticks)
    c.max_ticks = c.stage == 1 ? 100 : 500;
  if (c.max_ticks < 1 || c.max_ticks > 100000 || c.timeout_ms < 100 ||
      c.timeout_ms > 30000 || c.budget < 1 || c.budget > 1000 ||
      c.max_tokens < 128 || c.max_tokens > 16384)
    throw Error("option out of range");
  c.strategy = read_decision(c.decision_file);
  return c;
}
} // namespace rmtc
