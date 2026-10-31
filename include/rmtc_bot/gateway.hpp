/*
 * 文件：include/rmtc_bot/gateway.hpp
 * 作用：模型网关的 C++ HTTP 客户端（接口声明；具体行为见对应 .cpp）
 * 向官方/练习网关的 /v1/chat/completions
 * 发请求；处理令牌、超时、额度、重试和后台任务。
 * 输入感知快照、公告、网关配置；输出原始模型回复/诊断。LLMWorker
 * 后台运行，结果交 controller/events 校验。
 * 配套文件：src/gateway.cpp。这里是发请求的客户端，不是模型服务端；地址/令牌配置见
 * .env.example。
 */
#pragma once
#include "events.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
namespace rmtc {
std::string system_prompt();
struct GatewayError : Error {
  int status;
  bool retryable;
  GatewayError(std::string code, int s = 0, bool r = false)
      : Error(std::move(code)), status(s), retryable(r) {}
};
class Gateway {
  std::string endpoint_, model_;
  std::function<std::string()> token_;
  int timeout_ms_, budget_, max_tokens_;
  std::map<std::string, int> successes_, attempts_;
  std::set<std::string> disabled_;

public:
  Gateway(std::string endpoint, std::function<std::string()> token,
          std::string model = "", int timeout_ms = 4000, int budget = 48,
          int max_tokens = 1600);
  std::pair<std::string, int>
  ask(const Json &snapshot, const std::string &notices,
      const std::optional<std::string> &credential = std::nullopt);
  // 仅供后台线程读取本地令牌；令牌不会进入日志或公告去重键。
  std::string credential() const { return token_(); }
  const std::string &endpoint() const { return endpoint_; }
};
class LLMWorker {
  struct Job {
    Json snapshot;
    std::string text;
    std::string key;
    int retries = 0;
  };
  Gateway gateway_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopped_ = false;
  std::string session_;
  std::optional<Job> pending_;
  std::set<std::string> seen_;
  // 失败公告只对当时的令牌去重；裁判换发令牌后允许恢复，成功公告不重复收费。
  std::map<std::string, std::string> failed_credentials_;
  std::deque<Json> results_;
  std::thread thread_;
  int interval_ms_;
  void run();

public:
  LLMWorker(Gateway gateway, int interval_ms = 1000);
  ~LLMWorker();
  void reset(const std::string &session);
  void submit(const Frame &);
  std::vector<Json> drain();
  void close();
};
} // namespace rmtc
