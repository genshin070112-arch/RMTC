/*
 * 文件：src/gateway.cpp
 * 作用：模型网关的 C++ HTTP 客户端（实现文件）
 * 向官方/练习网关的 /v1/chat/completions
 * 发请求；处理令牌、超时、额度、重试和后台任务。
 * 输入感知快照、公告、网关配置；输出原始模型回复/诊断。LLMWorker
 * 后台运行，结果交 controller/events 校验。
 * 配套文件：include/rmtc_bot/gateway.hpp。这里是发请求的客户端，不是模型服务端；地址/令牌配置见
 * .env.example。
 */
#include "rmtc_bot/gateway.hpp"
#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <sstream>
namespace rmtc {
namespace {
struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};
CurlGlobal global_curl;
size_t receive(char *ptr, size_t size, size_t count, void *userdata) {
  auto *body = static_cast<std::string *>(userdata);
  size_t n = size * count;
  if (n > 2097152 || body->size() + n > 2097152)
    return 0;
  body->append(ptr, n);
  return n;
}
std::string url_part(CURLU *u, CURLUPart part) {
  char *text = nullptr;
  std::string out;
  if (curl_url_get(u, part, &text, 0) == CURLUE_OK) {
    out = text;
    curl_free(text);
  }
  return out;
}
} // namespace
Gateway::Gateway(std::string endpoint, std::function<std::string()> token,
                 std::string model, int timeout_ms, int budget, int max_tokens)
    : model_(std::move(model)), token_(std::move(token)),
      timeout_ms_(timeout_ms), budget_(budget), max_tokens_(max_tokens) {
  CURLU *u = curl_url();
  if (!u)
    throw Error("cannot allocate URL parser");
  if (curl_url_set(u, CURLUPART_URL, endpoint.c_str(), 0) != CURLUE_OK) {
    curl_url_cleanup(u);
    throw Error("invalid gateway URL");
  }
  auto scheme = url_part(u, CURLUPART_SCHEME),
       host = url_part(u, CURLUPART_HOST), path = url_part(u, CURLUPART_PATH);
  bool ok = (scheme == "http" || scheme == "https") && !host.empty() &&
            url_part(u, CURLUPART_USER).empty() &&
            url_part(u, CURLUPART_PASSWORD).empty() &&
            url_part(u, CURLUPART_QUERY).empty() &&
            url_part(u, CURLUPART_FRAGMENT).empty() &&
            (path.empty() || path == "/" || path == "/v1/chat/completions");
  if (!ok) {
    curl_url_cleanup(u);
    throw Error("gateway requires base URL or /v1/chat/completions without "
                "credentials/query/fragment");
  }
  curl_url_set(u, CURLUPART_PATH, "/v1/chat/completions", 0);
  endpoint_ = url_part(u, CURLUPART_URL);
  curl_url_cleanup(u);
  if (timeout_ms_ < 100 || timeout_ms_ > 30000 || budget_ < 1 ||
      budget_ > 1000 || max_tokens_ < 128 || max_tokens_ > 16384)
    throw Error("invalid gateway limits");
}
std::pair<std::string, int>
Gateway::ask(const Json &snapshot, const std::string &notices,
             const std::optional<std::string> &credential) {
  std::string token = credential ? *credential : token_();
  if (token.empty())
    throw GatewayError("missing_token", 401);
  if (token.size() > 4096 ||
      std::any_of(token.begin(), token.end(),
                  [](unsigned char c) { return std::iscntrl(c) != 0; }))
    throw GatewayError("invalid_token_format", 401);
  if (disabled_.count(token))
    throw GatewayError("token_disabled", 429);
  if (successes_[token] >= budget_ ||
      attempts_[token] >= std::max(4, budget_ * 2))
    throw GatewayError("local_budget_exhausted", 429);
  Json input = snapshot;
  input["notices"] = notices;
  Json body = {{"messages",
                Json::array({{{"role", "system"}, {"content", system_prompt()}},
                             {{"role", "user"}, {"content", input.dump()}}})},
               {"stream", false},
               {"temperature", 0},
               {"max_tokens", max_tokens_}};
  if (!model_.empty())
    body["model"] = model_;
  std::string payload = body.dump();
  if (payload.size() > 2097152)
    throw GatewayError("request_too_large", 413);
  CURL *curl = curl_easy_init();
  if (!curl)
    throw GatewayError("client_init_failed");
  std::string response;
  curl_slist *headers = nullptr;
  for (const auto &h : std::vector<std::string>{
           "Content-Type: application/json", "Accept: application/json",
           "Authorization: Bearer " + token, "Expect:"})
    headers = curl_slist_append(headers, h.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   std::min(1500, timeout_ms_));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  ++attempts_[token];
  CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (status >= 200 && status < 300)
    ++successes_[token]; // unusable content still consumes successful-forward
                         // quota
  if (result != CURLE_OK)
    throw GatewayError("network_or_timeout", static_cast<int>(status), true);
  if (status < 200 || status >= 300) {
    std::string code = "http_error";
    try {
      auto j = strict_json(response, 65536);
      if (j.contains("error") && j["error"].is_object() &&
          j["error"].contains("code") && j["error"]["code"].is_string()) {
        auto c = j["error"]["code"].get<std::string>();
        if (c.size() < 64 &&
            std::all_of(c.begin(), c.end(), [](unsigned char x) {
              return std::isalnum(x) || x == '_';
            }))
          code = c;
      }
    } catch (const std::exception &) {
    }
    if (status == 401 || code == "quota_exceeded")
      disabled_.insert(token);
    bool retry = (status == 429 || status == 500 || status == 502 ||
                  status == 503 || status == 504) &&
                 code != "quota_exceeded";
    throw GatewayError(code, static_cast<int>(status), retry);
  }
  try {
    auto j = strict_json(response);
    return {
        j.at("choices").at(0).at("message").at("content").get<std::string>(),
        static_cast<int>(status)};
  } catch (const std::exception &) {
    throw GatewayError("malformed_completion", static_cast<int>(status));
  }
}
LLMWorker::LLMWorker(Gateway gateway, int interval_ms)
    : gateway_(std::move(gateway)), interval_ms_(interval_ms) {
  thread_ = std::thread([this] { run(); });
}
LLMWorker::~LLMWorker() { close(); }
void LLMWorker::reset(const std::string &session) {
  std::lock_guard<std::mutex> lock(mutex_);
  session_ = session;
  pending_.reset();
  seen_.clear();
  failed_credentials_.clear();
}
void LLMWorker::submit(const Frame &f) {
  Json input = {{"session_id", f.session},
                {"seat", f.seat},
                {"tick", f.tick},
                {"map", {{"width", f.width}, {"height", f.height}}},
                {"missions", f.missions},
                // 直接传裁判本帧已结算积分；模型不得再次加奖/扣除，也不可假设 D
                // 单调增加。
                {"score", f.score},
                {"self",
                 {{"position", {f.position.first, f.position.second}},
                  {"hp", f.hp},
                  {"alive", f.alive},
                  {"protected", f.protected_spawn}}}};
  std::lock_guard<std::mutex> lock(mutex_);
  std::string key = std::to_string(std::hash<std::string>{}(f.event_text)) +
                    ":" + std::to_string(f.event_text.size());
  if (f.session != session_)
    return;
  std::string text = f.event_text;
  if (text.size() > 24000) {
    size_t begin = text.size() - 24000;
    while (begin < text.size() &&
           (static_cast<unsigned char>(text[begin]) & 0xC0) == 0x80)
      ++begin;
    text = text.substr(begin);
    input["notices_truncated"] = true;
  }
  pending_ = Job{input, text, key, 0};
  cv_.notify_one();
}
std::vector<Json> LLMWorker::drain() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Json> out(results_.begin(), results_.end());
  results_.clear();
  return out;
}
void LLMWorker::run() {
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now() - std::chrono::seconds(2);
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopped_) {
    cv_.wait(lock, [&] { return stopped_ || pending_.has_value(); });
    if (stopped_)
      break;
    Job job = *pending_;
    pending_.reset();
    std::string session = job.snapshot["session_id"];
    if (session != session_)
      continue;
    // 在后台读取令牌：即使公告未变化，新令牌也可恢复曾失败的请求。
    // 成功公告始终去重，不因换令牌而再次消耗额度。
    lock.unlock();
    std::string credential;
    try {
      credential = gateway_.credential();
    } catch (...) {
    }
    lock.lock();
    if (session != session_)
      continue;
    if (job.retries == 0 && seen_.count(job.key)) {
      auto failed = failed_credentials_.find(job.key);
      if (failed == failed_credentials_.end() || failed->second == credential)
        continue;
      seen_.erase(job.key);
      failed_credentials_.erase(failed);
    }
    auto when = last + std::chrono::milliseconds(interval_ms_);
    cv_.wait_until(lock, when, [&] { return stopped_; });
    if (stopped_)
      break;
    if (session != session_)
      continue;
    last = Clock::now();
    lock.unlock();
    int status = 0;
    bool retry = false;
    int retry_limit = 3;
    std::string detail;
    Json parsed = nullptr;
    std::string error;
    try {
      auto answer = gateway_.ask(job.snapshot, job.text, credential);
      status = answer.second;
      try {
        parsed =
            model_output(answer.first, session, job.snapshot["map"]["width"],
                         job.snapshot["map"]["height"]);
        if (!parsed.value("validation_warnings", Json::array()).empty()) {
          error = "model_schema_partial";
          retry_limit = 2;
          retry = job.retries < retry_limit;
        }
      } catch (const std::exception &e) {
        error = "model_schema_invalid";
        detail = std::string(e.what()).substr(0, 160);
        retry_limit = 2;
        retry = job.retries < retry_limit;
      }
    } catch (const GatewayError &e) {
      error = e.what();
      status = e.status;
      retry = e.retryable;
    } catch (const std::exception &) {
      error = "client_internal_error";
    }
    double elapsed =
        std::chrono::duration<double, std::milli>(Clock::now() - last).count();
    lock.lock();
    if (session == session_) {
      if (!retry || job.retries >= retry_limit) {
        seen_.insert(job.key);
        if (parsed.is_null())
          failed_credentials_[job.key] = credential;
        else
          failed_credentials_.erase(job.key);
      }
      if (seen_.size() > 256) {
        failed_credentials_.erase(*seen_.begin());
        seen_.erase(seen_.begin());
      }
      // 重试中的部分语义只供诊断；否则更正 start_tick 后旧事件仍会留在事件表。
      const bool provisional = retry && job.retries < retry_limit;
      results_.push_back({{"kind", "llm_result"},
                          {"session_id", session},
                          {"source_tick", job.snapshot["tick"]},
                          {"status", status},
                          {"error", error},
                          {"detail", detail},
                          {"attempt", job.retries + 1},
                          {"provisional", provisional},
                          {"parsed", provisional ? Json(nullptr) : parsed},
                          {"duration_ms", elapsed},
                          {"notice_bytes", job.text.size()}});
      if (results_.size() > 128)
        results_.pop_front();
      if (retry && job.retries < retry_limit) {
        cv_.wait_for(lock, std::chrono::seconds(1 << job.retries),
                     [&] { return stopped_ || session != session_; });
        if (!stopped_ && session == session_ &&
            (!pending_ || pending_->key == job.key)) {
          ++job.retries;
          pending_ = job;
          cv_.notify_one();
        }
      }
    }
  }
}
void LLMWorker::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }
  if (thread_.joinable())
    thread_.join();
}
} // namespace rmtc
