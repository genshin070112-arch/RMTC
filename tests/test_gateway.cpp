/*
 * 文件：tests/test_gateway.cpp
 * 作用：网关故障与异步行为测试
 * 启动本地 HTTP 测试夹具，检查请求、额度、超时、限流、重试、去重、UTF-8
 * 与跨场隔离。 由 CTest 调用；输出
 * PASS/FAIL。夹具回复是测试数据，不是实际模型语义能力的证据。
 * 配套：src/gateway.cpp；运行同 test_core.cpp。
 */
#include "rmtc_bot/gateway.hpp"
#include "rmtc_bot/controller.hpp"
#include <atomic>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace rmtc;
namespace {
struct Response {
  int status = 200;
  std::string body;
  int delay_ms = 0;
  std::string extra;
};
class Server {
  int fd_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::function<Response(const std::string &)> handler_;

public:
  int port = 0;
  std::atomic<int> calls{0};
  std::string last_request;
  explicit Server(std::function<Response(const std::string &)> h)
      : handler_(std::move(h)) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 ||
        listen(fd_, 8) != 0)
      throw Error("fixture bind failed");
    socklen_t len = sizeof addr;
    getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len);
    port = ntohs(addr.sin_port);
    thread_ = std::thread([this] {
      while (!stop_) {
        int c = accept(fd_, nullptr, nullptr);
        if (c < 0)
          break;
        timeval timeout{2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        std::string request;
        char buffer[8192];
        size_t want = 0;
        while (request.size() < 2097152) {
          ssize_t n = recv(c, buffer, sizeof buffer, 0);
          if (n <= 0)
            break;
          request.append(buffer, n);
          auto header = request.find("\r\n\r\n");
          if (header != std::string::npos) {
            auto p = request.find("Content-Length: ");
            if (p != std::string::npos)
              want = std::stoul(request.substr(p + 16));
            if (request.size() >= header + 4 + want)
              break;
          }
        }
        ++calls;
        last_request = request;
        Response r = handler_(request);
        if (r.delay_ms)
          std::this_thread::sleep_for(std::chrono::milliseconds(r.delay_ms));
        std::string body =
            "HTTP/1.1 " + std::to_string(r.status) +
            " Test\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(r.body.size()) + "\r\nConnection: close\r\n" +
            r.extra + "\r\n" + r.body;
        send(c, body.data(), body.size(), MSG_NOSIGNAL);
        close(c);
      }
    });
  }
  ~Server() {
    stop_ = true;
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
    thread_.join();
  }
  std::string url() { return "http://127.0.0.1:" + std::to_string(port); }
};
int passed = 0, failed = 0;
void check(bool x, const std::string &why) {
  if (!x)
    throw Error(why);
}
void test(const std::string &n, const std::function<void()> &f) {
  try {
    f();
    ++passed;
    std::cout << "PASS " << n << std::endl;
  } catch (const std::exception &e) {
    ++failed;
    std::cout << "FAIL " << n << ": " << e.what() << std::endl;
  }
}
std::string completion(const std::string &session) {
  return Json({{"choices",
                Json::array({{{"message",
                               {{"content", Json({{"session_id", session},
                                                  {"events", Json::array()},
                                                  {"effects", Json::array()},
                                                  {"goal", nullptr}})
                                                .dump()}}}}})}})
      .dump();
}
Json snapshot() {
  return {{"session_id", "s1"},
          {"seat", "A"},
          {"tick", 1},
          {"map", {{"width", 20}, {"height", 20}}}};
}
Frame frame(const std::string &s) {
  Frame f;
  f.session = s;
  f.seat = "A";
  f.width = f.height = 20;
  f.hp = 100;
  f.missions = Json::array();
  f.event_text = "任意中文公告，无本地关键词解析";
  return f;
}
std::vector<Json> await(LLMWorker &worker, int milliseconds = 3000) {
  auto end = std::chrono::steady_clock::now() +
             std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < end) {
    auto rows = worker.drain();
    if (!rows.empty())
      return rows;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw Error("worker result timeout");
}
} // namespace
int main() {
  test("third_report_48_success_budget_is_enforced_without_silent_extra_calls", [] {
    Server s([](const auto &){return Response{200,completion("s1")};});
    Gateway g(s.url(),[]{return "offline-budget";},"",1000,48);
    for(int i=0;i<48;++i)g.ask(snapshot(),"announcement "+std::to_string(i));
    bool rejected=false;
    try{g.ask(snapshot(),"49th");}catch(const GatewayError &e){rejected=std::string(e.what())=="local_budget_exhausted";}
    check(rejected && s.calls==48,"48 cap bypassed or stopped early");
  });

  test("partial_retry_publishes_only_final_facts", [] {
    std::atomic<int> attempts{0};
    Server s([&](const auto &) {
      int n=++attempts;
      Json events=Json::array({{{"kind","explosion"},{"start_tick",n==1?10:20},{"end_tick",n==1?11:21},{"rect",{1,1,2,2}}}});
      if(n==1)events.push_back({{"kind","bad"}});
      auto result=Json({{"session_id","s1"},{"events",events},{"effects",Json::array()},{"goal",nullptr}});
      return Response{200,Json({{"choices",Json::array({{{"message",{{"content",result.dump()}}}}})}}).dump()};
    });
    LLMWorker w(Gateway(s.url(),[]{return "test";},"",1000,8),10);w.reset("s1");w.submit(frame("s1"));
    bool stale=false,final=false;int published=0;
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<until && !final) {
      for(auto row:w.drain()) if(!row["parsed"].is_null()) {
        ++published;for(auto event:row["parsed"]["events"]) {stale|=event["start_tick"]==10;final|=event["start_tick"]==20;}
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(final && !stale && published==1 && s.calls==2,"partial attempt escaped before corrected final result");
  });

  test("partial_retry_terminal_partial_published_once", [] {
    std::atomic<int> attempts{0};
    Server s([&](const auto &) {
      int start = 10 * ++attempts;
      Json facts = {{"session_id", "s1"}, {"effects", Json::array()}, {"goal", nullptr},
        {"events", Json::array({{{"kind", "explosion"}, {"start_tick", start},
             {"end_tick", start + 1}, {"rect", {1,1,2,2}}}, {{"kind", "bad"}}})}};
      return Response{200, Json({{"choices", Json::array({{{"message", {{"content", facts.dump()}}}}})}}).dump()};
    });
    LLMWorker w(Gateway(s.url(), [] { return "fixture"; }, "", 1000, 8), 10);
    w.reset("s1"); w.submit(frame("s1"));
    int published = 0, diagnostics = 0; bool terminal = false;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!terminal && std::chrono::steady_clock::now() < until) {
      for (const auto &r : w.drain()) {
        if (r.at("parsed").is_null()) {
          ++diagnostics; check(r.at("provisional") == true, "retry must be diagnostic");
        } else {
          ++published;
          check(r.at("attempt") == 3 && r.at("provisional") == false &&
                r.at("parsed").at("events").at(0).at("start_tick") == 30,
                "only the terminal partial facts may be accepted");
          terminal = true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(terminal && published == 1 && diagnostics == 2 && attempts == 3,
          "bounded partial retry result sequence");
  });
  // 合成的已知正确事件经过真实 HTTP/异步队列/Controller 后参与决策。
  // 不测试真实模型的语言理解，也不接触裁判隐藏信息。
  for (const std::string kind : {"angel", "unrivaled", "explosion"})
    test("fixture_http_to_action_" + kind, [kind] {
      Json event;
      if (kind == "angel")
        event = {{"kind",kind},{"start_tick",10},{"end_tick",30},
                 {"points",Json::array({{8,9}})},{"heal_per_tick",10},{"effect_ticks",10}};
      else if (kind == "unrivaled")
        event = {{"kind",kind},{"start_tick",10},{"end_tick",60},
                 {"center",{8,8}},{"size",3},{"stay_ticks",10},{"damage",50}};
      else
        event = {{"kind",kind},{"start_tick",23},{"end_tick",24},
                 {"rect",{7,7,9,9}},{"damage",80}};
      Json facts = {{"session_id","s1"},{"events",Json::array({event})},
                    {"effects",Json::array()},{"goal",nullptr}};
      Server s([&](const auto &) {
        return Response{200, Json({{"choices",Json::array({{{"message",{{"content",facts.dump()}}}}})}}).dump()};
      });
      Json raw = {{"tick",20},{"session_id","s1"},{"vision_radius",4},
        {"map",{{"width",20},{"height",20}}},
        {"self",{{"team","A"},{"position",{8,8}},{"hp",kind == "angel" ? 20 : 100},
                  {"alive",true},{"protected",false},{"defending",false},{"last_action",nullptr}}},
        {"visible_entities",Json::array()},{"missions",Json::array()},
        {"score",{{"resource_value",0},{"effective_damage",0},{"invalid_actions",0},
                   {"mission_completed_ids",Json::array()}}},{"event_text","离线语义夹具"}};
      auto f = parse_frame(raw.dump(), "A");
      Controller c("A",2,500); c.handle(f);
      LLMWorker w(Gateway(s.url(), [] { return "fixture"; }, "",1000,8),10);
      w.reset("s1"); w.submit(f);
      auto results = await(w);
      check(results.size()==1 && c.ingest(results.front(),20), "event not ingested");
      raw["tick"] = 21;
      auto a = c.handle(parse_frame(raw.dump(),"A")).first;
      const std::string expected = kind == "angel" ? "seek_announced_healing" :
          kind == "unrivaled" ? "hold_announced_fortress" : "escape_announced_event";
      check(c.planner->reason == expected, "valid event did not influence decision: " + c.planner->reason);
      check(a.at("action") == (kind == "unrivaled" ? "defend" : "move"), "wrong event action");
      if (kind == "angel") check(a.at("direction") == "down", "did not enter angel point");
    });

  test("http_payload_auth_and_content_length", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1")};
    });
    Gateway g(s.url(), [] { return "test-secret"; });
    auto result = g.ask(snapshot(), "引号\"换行\n中文");
    check(result.second == 200, "status");
    check(s.last_request.find("POST /v1/chat/completions ") !=
              std::string::npos,
          "wrong path");
    check(s.last_request.find("Authorization: Bearer test-secret") !=
              std::string::npos,
          "no bearer");
    check(s.last_request.find("Content-Length:") != std::string::npos &&
              s.last_request.find("Transfer-Encoding:") == std::string::npos,
          "chunked request");
    auto p = s.last_request.find("\r\n\r\n");
    auto body = strict_json(s.last_request.substr(p + 4));
    check(body["stream"] == false && body["messages"].size() == 2,
          "request schema");
    model_output(result.first, "s1", 20, 20);
  });
  test("success_budget_and_lifetime_not_session", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1")};
    });
    Gateway g(
        s.url(), [] { return "same-token"; }, "", 1000, 1);
    g.ask(snapshot(), "");
    bool rejected = false;
    try {
      auto j = snapshot();
      j["session_id"] = "s2";
      g.ask(j, "");
    } catch (const GatewayError &e) {
      rejected = std::string(e.what()) == "local_budget_exhausted";
    }
    check(rejected && s.calls == 1, "budget reset on session");
  });
  test("v11_worker_forwards_latest_score_snapshot", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1")};
    });
    LLMWorker worker(Gateway(s.url(), [] { return "token"; }), 1);
    worker.reset("s1");
    auto f = frame("s1");
    f.score = {{"effective_damage", 100.5},
               {"mission_completed_ids", Json::array({"damage_done"})}};
    worker.submit(f);
    await(worker);
    worker.close();
    auto start = s.last_request.find("\r\n\r\n");
    auto body = strict_json(s.last_request.substr(start + 4));
    // user 消息的 JSON 为实际模型输入；校验不是只测内部 Frame。
    auto input = strict_json(body["messages"][1]["content"].get<std::string>());
    check(input["score"] == f.score, "latest referee score not forwarded");
  });
  test("malformed_2xx_still_consumes_quota", [] {
    Server s([](const std::string &) { return Response{200, "{}"}; });
    Gateway g(
        s.url(), [] { return "token"; }, "", 1000, 1);
    try {
      g.ask(snapshot(), "");
    } catch (const GatewayError &e) {
      check(e.status == 200, "lost 2xx");
    }
    try {
      g.ask(snapshot(), "");
      throw Error("no budget rejection");
    } catch (const GatewayError &e) {
      check(std::string(e.what()) == "local_budget_exhausted",
            "refunded malformed response");
    }
  });
  test("quota_exceeded_terminal", [] {
    Server s([](const std::string &) {
      return Response{429, "{\"error\":{\"code\":\"quota_exceeded\"}}"};
    });
    Gateway g(s.url(), [] { return "token"; });
    for (int i = 0; i < 2; ++i)
      try {
        g.ask(snapshot(), "");
        throw Error("no error");
      } catch (const GatewayError &e) {
        check(!e.retryable && e.status == 429, "quota retried");
      }
    check(s.calls == 1, "disabled token sent again");
  });
  test("rate_limit_retryable", [] {
    Server s([](const std::string &) {
      return Response{429, "{\"error\":{\"code\":\"rate_limit\"}}"};
    });
    Gateway g(s.url(), [] { return "token"; });
    try {
      g.ask(snapshot(), "");
      throw Error("no error");
    } catch (const GatewayError &e) {
      check(e.retryable && e.status == 429, "rate limit not retryable");
    }
  });
  test("401_disables_token", [] {
    Server s([](const std::string &) { return Response{401, "not json"}; });
    Gateway g(s.url(), [] { return "token"; });
    try {
      g.ask(snapshot(), "");
    } catch (const GatewayError &e) {
      check(!e.retryable, "auth retry");
    }
    try {
      g.ask(snapshot(), "");
    } catch (const GatewayError &) {
    }
    check(s.calls == 1, "auth kept sending");
  });
  test("redirect_not_followed", [] {
    Server s([](const std::string &) {
      return Response{302, "{}", 0, "Location: http://127.0.0.1:1/steal\r\n"};
    });
    Gateway g(s.url(), [] { return "token"; });
    try {
      g.ask(snapshot(), "");
      throw Error("redirect followed");
    } catch (const GatewayError &e) {
      check(e.status == 302 && !e.retryable, "redirect behavior");
    }
  });
  test("timeout_bounded", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1"), 250};
    });
    Gateway g(
        s.url(), [] { return "token"; }, "", 100);
    auto start = std::chrono::steady_clock::now();
    try {
      g.ask(snapshot(), "");
      throw Error("no timeout");
    } catch (const GatewayError &e) {
      check(e.retryable, "timeout no retry");
    }
    check(std::chrono::steady_clock::now() - start <
              std::chrono::milliseconds(220),
          "blocked too long");
  });
  test("500_non_json_handled", [] {
    Server s([](const std::string &) {
      return Response{500, "<html>failure</html>"};
    });
    Gateway g(s.url(), [] { return "token"; });
    try {
      g.ask(snapshot(), "");
      throw Error("missing failure");
    } catch (const GatewayError &e) {
      check(e.status == 500 && e.retryable, "500 parse crashed");
    }
  });
  test("worker_nonblocking_and_deduplicated", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1"), 150};
    });
    LLMWorker w(Gateway(s.url(), [] { return "token"; }), 10);
    w.reset("s1");
    auto f = frame("s1");
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i)
      w.submit(f);
    check(std::chrono::steady_clock::now() - start <
              std::chrono::milliseconds(30),
          "submit blocked");
    auto rows = await(w);
    check(rows[0]["status"] == 200 && !rows[0]["parsed"].is_null(),
          "worker parse");
    for (int i = 0; i < 100; ++i)
      w.submit(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    check(s.calls == 1, "duplicate calls");
  });
  test("stale_inflight_session_discarded", [] {
    Server s([](const std::string &request) {
      auto p = request.find("\r\n\r\n");
      auto body = strict_json(request.substr(p + 4));
      auto input =
          strict_json(body["messages"][1]["content"].get<std::string>());
      return Response{200, completion(input["session_id"].get<std::string>()),
                      150};
    });
    LLMWorker w(Gateway(s.url(), [] { return "token"; }), 10);
    w.reset("old");
    w.submit(frame("old"));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!s.calls && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    w.reset("new");
    w.submit(frame("new"));
    auto rows = await(w);
    check(rows.size() == 1 && rows[0]["session_id"] == "new",
          "old result leaked");
  });

  test("worker_retry_survives_repeated_perceptions", [] {
    std::atomic<int> request_count{0};
    Server s([&](const std::string &) {
      if (++request_count == 1)
        return Response{429, "{\"error\":{\"code\":\"rate_limit\"}}", 150};
      return Response{200, completion("s1")};
    });
    LLMWorker w(Gateway(s.url(), [] { return "token"; }), 10);
    w.reset("s1");
    auto f = frame("s1");
    w.submit(f);
    for (int i = 0; i < 50; ++i) {
      w.submit(f);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto first = await(w);
    bool good = false;
    for (const auto &row : first)
      if (row["status"] == 200)
        good = true;
    if (!good) {
      auto next = await(w, 2500);
      for (const auto &row : next)
        if (row["status"] == 200)
          good = true;
    }
    check(good && s.calls == 2, "transient retry was lost");
  });
  test("long_utf8_notice_valid_and_deduplicated", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1")};
    });
    LLMWorker w(Gateway(s.url(), [] { return "token"; }), 10);
    w.reset("s1");
    auto f = frame("s1");
    f.event_text = "X";
    for (int i = 0; i < 10000; ++i)
      f.event_text += "爆";
    w.submit(f);
    auto rows = await(w);
    check(rows[0]["status"] == 200 && !rows[0]["parsed"].is_null(),
          "UTF-8 was split");
    for (int i = 0; i < 20; ++i)
      w.submit(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    check(s.calls == 1, "truncated notice lost deduplication identity");
  });
  test("missing_token_then_refresh_same_notice", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1")};
    });
    std::atomic<bool> ready{false};
    LLMWorker w(
        Gateway(s.url(), [&] { return ready.load() ? "new-token" : ""; }), 10);
    w.reset("s1");
    auto f = frame("s1");
    w.submit(f);
    auto first = await(w);
    check(first[0]["error"] == "missing_token",
          "expected original missing token");
    ready = true;
    ++f.tick;
    w.submit(f);
    auto second = await(w, 500);
    check(second[0]["status"] == 200 && s.calls == 1,
          "token refresh never retried unchanged notice");
  });
  test("401_then_refresh_same_notice", [] {
    Server s([](const std::string &request) {
      if (request.find("Bearer old-token") != std::string::npos)
        return Response{401, "{\"error\":{\"code\":\"invalid_token\"}}"};
      return Response{200, completion("s1")};
    });
    std::atomic<bool> refreshed{false};
    LLMWorker w(
        Gateway(s.url(),
                [&] { return refreshed.load() ? "new-token" : "old-token"; }),
        10);
    w.reset("s1");
    auto f = frame("s1");
    w.submit(f);
    auto first = await(w);
    check(first[0]["status"] == 401, "expected original 401");
    refreshed = true;
    ++f.tick;
    w.submit(f);
    auto second = await(w, 500);
    check(second[0]["status"] == 200 && s.calls == 2,
          "token refresh never retried unchanged notice");
  });
  test("successful_notice_stays_deduplicated_after_token_rotation", [] {
    Server s([](const std::string &) {
      return Response{200, completion("s1")};
    });
    std::atomic<bool> refreshed{false};
    LLMWorker w(
        Gateway(s.url(),
                [&] { return refreshed.load() ? "new-token" : "old-token"; }),
        1);
    w.reset("s1");
    auto f = frame("s1");
    w.submit(f);
    auto first = await(w);
    check(first[0]["status"] == 200, "initial success");
    refreshed = true;
    ++f.tick;
    w.submit(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    w.close();
    check(s.calls == 1 && w.drain().empty(),
          "successful notice charged twice after token rotation");
  });
  test("schema_error_retries_then_succeeds", [] {
    std::atomic<int> attempts{0};
    Server s([&](const auto &) {
      return ++attempts == 1 ? Response{200, "{\"choices\":[{\"message\":{"
                                             "\"content\":\"invalid json\"}}]}"}
                             : Response{200, completion("s1")};
    });
    LLMWorker w(Gateway(
                    s.url(), [] { return "x"; }, "", 1000, 8),
                10);
    w.reset("s1");
    w.submit(frame("s1"));
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    bool parsed = false;
    while (std::chrono::steady_clock::now() < end && !parsed) {
      for (const auto &r : w.drain())
        if (!r["parsed"].is_null())
          parsed = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(parsed && s.calls == 2, "schema failure never retried");
  });
  test("schema_retries_are_bounded_even_with_continuous_frames", [] {
    Server s([](const auto &) {
      return Response{200,
                      "{\"choices\":[{\"message\":{\"content\":\"bad\"}}]}"};
    });
    LLMWorker w(Gateway(
                    s.url(), [] { return "x"; }, "", 1000, 8),
                10);
    w.reset("s1");
    auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(3600);
    while (std::chrono::steady_clock::now() < end) {
      w.submit(frame("s1"));
      w.drain();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(s.calls == 3, "schema retry cap not 1+2");
  });
  std::cout << "RESULT " << passed << " passed, " << failed << " failed"
            << std::endl;
  return failed ? 1 : 0;
}
