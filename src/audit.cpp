/*
 * 文件：src/audit.cpp
 * 作用：异步运行日志（实现文件）
 * 通过有界队列与独立写入线程记录 JSONL；避免主控制回调等待磁盘写入。
 * 输入 node 产生的诊断记录；输出 logs/ 中的 bot-*.jsonl，不记录令牌。
 * 配套文件：include/rmtc_bot/audit.hpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#include "rmtc_bot/audit.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
namespace rmtc {
Audit::Audit(const std::string &dir) {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  path =
      (std::filesystem::path(dir) /
       ("bot-" +
        std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) +
        ".jsonl"))
          .string();
  // 日志故障不应阻止合法动作。只降级记录能力，并显式报告证据缺口。
  try {
    std::filesystem::create_directories(dir);
    file_.open(path);
    if (!file_)
      throw Error("cannot create bot audit log");
  } catch (const std::exception &) {
    error = true;
    std::cerr << "RMTC audit unavailable; local actions remain enabled; "
                 "file evidence will be incomplete"
              << std::endl;
  }
  writer_ = std::thread([this] { run(); });
}
Audit::~Audit() { close(); }
void Audit::write(Json j) {
  j["monotonic_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= 8192) {
    error = true;
    return;
  }
  queue_.push_back(std::move(j));
  cv_.notify_one();
}
void Audit::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopped_ || !queue_.empty()) {
    cv_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
    if (queue_.empty())
      continue;
    Json j = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    try {
      auto line = j.dump();
      if (file_.is_open() && file_) {
        file_ << line << '\n';
        // 在专用线程刷新，降低异常退出时缓冲区中证据丢失，也及时发现磁盘满。
        file_.flush();
      }
      if (!file_) {
        if (!error.exchange(true))
          std::cerr << "RMTC audit write failed; local actions remain enabled; "
                       "file evidence will be incomplete"
                    << std::endl;
        error = true;
      }
      auto kind = j.value("kind", "");
      if (kind == "ready" || kind == "session" || kind == "llm_result" ||
          kind == "error" || kind == "warning" || kind == "watchdog" ||
          kind == "strategy" || kind == "summary")
        std::cout << line << std::endl;
    } catch (...) {
      error = true;
    }
    lock.lock();
  }
  file_.flush();
}
void Audit::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }
  if (writer_.joinable())
    writer_.join();
  if (file_.is_open())
    file_.close();
}
} // namespace rmtc
