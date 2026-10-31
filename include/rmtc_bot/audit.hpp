/*
 * 文件：include/rmtc_bot/audit.hpp
 * 作用：异步运行日志（接口声明；具体行为见对应 .cpp）
 * 通过有界队列与独立写入线程记录 JSONL；避免主控制回调等待磁盘写入。
 * 输入 node 产生的诊断记录；输出 logs/ 中的 bot-*.jsonl，不记录令牌。
 * 配套文件：src/audit.cpp。文件位置与配置入口见项目根目录《代码导航.md》。
 */
#pragma once
#include "protocol.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
namespace rmtc {
class Audit {
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Json> queue_;
  bool stopped_ = false;
  std::ofstream file_;
  std::thread writer_;
  void run();

public:
  std::string path;
  std::atomic<bool> error{false};
  explicit Audit(const std::string &directory);
  ~Audit();
  void write(Json);
  void close();
};
} // namespace rmtc
