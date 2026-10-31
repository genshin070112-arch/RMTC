/*
 * 文件：src/node.cpp
 * 作用：C++ 参赛程序入口与 ROS2 接线
 * main() 读取配置、启动 ROS2
 * 节点；发布注册/动作，订阅感知、任务与公告，调度控制器和日志。 输入 ROS2
 * 消息和配置；输出 ROS2 动作/注册与日志。后台模型请求交给 gateway。
 * 阅读顺序：config.cpp → node.cpp → controller.cpp →
 * planner.cpp；完整导航见《代码导航.md》。
 */
#include "rmtc_bot/audit.hpp"
#include "rmtc_bot/config.hpp"
#include "rmtc_bot/controller.hpp"
#include "rmtc_bot/gateway.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
namespace rmtc {
class Bot : public rclcpp::Node {
  using Message = std_msgs::msg::String;
  using Clock = std::chrono::steady_clock;
  Config config_;
  Audit audit_;
  Controller controller_;
  std::unique_ptr<LLMWorker> worker_;
  rclcpp::Publisher<Message>::SharedPtr action_pub_, registration_;
  rclcpp::Subscription<Message>::SharedPtr perception_, notices_, mission_;
  rclcpp::TimerBase::SharedPtr timer_;
  Clock::time_point last_publish_{}, last_frame_{}, started_ = Clock::now();
  std::filesystem::file_time_type config_stamp_{};
  std::set<int> sent_;
  int frames_ = 0, receipts_ = 0, switches_ = 0;
  bool warned_ = false, receipt_warned_ = false;
  int first_tick_ = -1, last_receipt_tick_ = -1;
  std::string mission_text_;
  void poll() {
    try {
      auto stamp = std::filesystem::last_write_time(config_.decision_file);
      if (stamp != config_stamp_) {
        auto mode = read_decision(config_.decision_file);
        config_stamp_ = stamp;
        if (mode != controller_.strategy) {
          if (!controller_.session.empty() && switches_ >= 2) {
            audit_.write(
                {{"kind", "error"}, {"code", "strategy_switch_limit"}});
            return;
          }
          if (!controller_.session.empty())
            ++switches_;
          controller_.change_strategy(mode);
          audit_.write({{"kind", "strategy"},
                        {"decision_type", mode},
                        {"manual_switches_this_session", switches_},
                        {"requires_referee_approval", true}});
        }
      }
    } catch (...) {
      audit_.write({{"kind", "error"}, {"code", "decision_config_invalid"}});
    }
    auto last = last_frame_ == Clock::time_point{} ? started_ : last_frame_;
    if (Clock::now() - last > std::chrono::seconds(3) && !warned_) {
      warned_ = true;
      audit_.write(
          {{"kind", "watchdog"},
           {"status", last_frame_ == Clock::time_point{}
                          ? "no_initial_perception_check_domain_seat_network"
                          : "no_recent_perception_no_stale_actions"}});
    }
  }
  void frame(const Message &message) {
    auto started = Clock::now();
    Frame f;
    try {
      f = parse_frame(message.data, config_.seat);
    } catch (const std::exception &e) {
      audit_.write({{"kind", "error"},
                    {"code", "perception_invalid"},
                    {"detail", e.what()}});
      return;
    }
    if (controller_.retired.count(f.session) ||
        (f.session == controller_.session &&
         (f.tick < controller_.tick || f.width != controller_.world->width ||
          f.height != controller_.world->height)))
      return;
    if (f.session != controller_.session) {
      controller_.reset(f);
      sent_.clear();
      first_tick_ = f.tick;
      last_receipt_tick_ = f.tick - 1;
      receipt_warned_ = false;
      switches_ = 0;
      if (worker_)
        worker_->reset(f.session);
      audit_.write({{"kind", "session"},
                    {"session_id", f.session},
                    {"position", {f.position.first, f.position.second}}});
    }
    for (const auto &warning : f.warnings)
      audit_.write({{"kind", "warning"}, {"code", warning}, {"tick", f.tick}});
    if (worker_)
      for (auto result : worker_->drain()) {
        bool accepted = false;
        try {
          accepted = controller_.ingest(result, f.tick);
        } catch (...) {
          result["error"] = "event_ingest_failed";
        }
        result["accepted"] = accepted;
        audit_.write(std::move(result));
      }
    Json a;
    std::string mode;
    try {
      auto out = controller_.handle(f);
      a = out.first;
      mode = out.second;
    } catch (const std::exception &e) {
      audit_.write({{"kind", "error"},
                    {"code", "planner_failure"},
                    {"detail", e.what()},
                    {"tick", f.tick}});
      // Frame关键字段已校验；不猜移动，缓存合法WAIT，避免重复帧推进或旧动作重放。
      a = f.tick < config_.max_ticks ? action(f, "wait", "", config_.stage)
                                     : Json();
      controller_.cached = a;
      controller_.planner->reason = "planner_failure_wait";
      mode = "fallback";
    }
    if (warned_)
      audit_.write({{"kind", "watchdog"},
                    {"status", "perception_resumed"},
                    {"tick", f.tick}});
    last_frame_ = Clock::now();
    warned_ = false;
    if (worker_ && mode == "new_tick" && f.tick < config_.max_ticks)
      worker_->submit(f);
    if (mode == "replay" &&
        Clock::now() - last_publish_ < std::chrono::milliseconds(50))
      return;
    if (mode == "old_tick" || mode == "map_changed")
      return;
    if (!a.is_null()) {
      Message m;
      m.data = a.dump();
      action_pub_->publish(m);
      last_publish_ = Clock::now();
      sent_.insert(f.tick);
    }
    bool observed = false;
    if (!f.receipt.is_null()) {
      int t = f.receipt["tick"];
      // E_BLOCKED/E_STATE 等失败回执仍证明双向通信畅通；执行成功与否另看原始回执。
      observed = sent_.count(t) && t < f.tick;
      if (observed) {
        ++receipts_;
        last_receipt_tick_ = std::max(last_receipt_tick_, t);
        receipt_warned_ = false;
      }
    }
    if (f.tick - first_tick_ >= 5 && f.tick - last_receipt_tick_ > 4 &&
        !receipt_warned_) {
      audit_.write(
          {{"kind", "watchdog"},
           {"status", "missing_action_receipts_check_bidirectional_DDS"},
           {"tick", f.tick}});
      receipt_warned_ = true;
    }
    ++frames_;
    double us =
        std::chrono::duration<double, std::micro>(Clock::now() - started)
            .count();
    Json completed = Json::array();
    for (const auto &id : f.completed)
      completed.push_back(id);
    Json goal = nullptr;
    if (controller_.planner->goal.first >= 0)
      goal = {controller_.planner->goal.first,
              controller_.planner->goal.second};
    audit_.write({{"kind", "frame"},
                  {"session_id", f.session},
                  {"tick", f.tick},
                  {"position", {f.position.first, f.position.second}},
                  {"hp", f.hp},
                  {"action", a},
                  {"mode", mode},
                  {"reason", controller_.planner->reason},
                  {"goal", goal},
                  {"score", f.score},
                  {"completed", completed},
                  {"last_action", f.receipt},
                  {"receipt_observed_for_sent_tick", observed},
                  {"planner_steps", controller_.steps},
                  {"known_resources", controller_.world->resources.size()},
                  {"known_cells", controller_.world->seen.size()},
                  {"events", controller_.events.events.size()},
                  {"processing_us", us}});
  }

public:
  explicit Bot(Config c)
      : Node("rmtc_cpp_" + c.seat), config_(std::move(c)),
        audit_(config_.log_dir),
        controller_(config_.seat, config_.stage, config_.max_ticks,
                    config_.strategy, config_.resource_cap) {
    if (config_.stage != 1)
      worker_ = std::make_unique<LLMWorker>(Gateway(
          config_.gateway, [p = config_.token_file] { return read_token(p); },
          config_.model, config_.timeout_ms, config_.budget,
          config_.max_tokens));
    auto qos =
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile();
    auto retained =
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
    action_pub_ =
        create_publisher<Message>("/team_" + config_.seat + "/action", qos);
    registration_ = create_publisher<Message>(
        "/team_" + config_.seat + "/registration", retained);
    Message registration;
    registration.data =
        Json({{"team", config_.seat}, {"name", config_.name}}).dump();
    registration_->publish(registration);
    perception_ = create_subscription<Message>(
        "/team_" + config_.seat + "/perception", qos,
        [this](Message::ConstSharedPtr m) { frame(*m); });
    notices_ = create_subscription<Message>(
        "/referee/event_text",
        rclcpp::QoS(rclcpp::KeepLast(128)).reliable().transient_local(),
        [this](Message::ConstSharedPtr m) {
          audit_.write(
              {{"kind", "notice_observed"}, {"bytes", m->data.size()}});
        });
    mission_ = create_subscription<Message>(
        "/referee/mission", retained, [this](Message::ConstSharedPtr m) {
          mission_text_ = m->data.substr(0, 24000);
        });
    timer_ =
        create_wall_timer(std::chrono::milliseconds(500), [this] { poll(); });
    audit_.write(
        {{"kind", "ready"},
         {"seat", config_.seat},
         {"stage", config_.stage},
         {"max_ticks", config_.max_ticks},
         {"ros_domain",
          std::getenv("ROS_DOMAIN_ID") ? std::getenv("ROS_DOMAIN_ID") : "0"},
         {"rmw", std::getenv("RMW_IMPLEMENTATION")
                     ? std::getenv("RMW_IMPLEMENTATION")
                     : "default"},
         {"strategy", config_.strategy},
         {"log", audit_.path},
         {"llm_configured",
          config_.stage != 1 && !read_token(config_.token_file).empty()}});
  }
  ~Bot() override {
    if (worker_)
      worker_->close();
    audit_.write({{"kind", "summary"},
                  {"frames", frames_},
                  {"receipt_observations", receipts_},
                  {"log_error_or_dropped", audit_.error.load()},
                  {"session_id", controller_.session}});
    audit_.close();
  }
};
} // namespace rmtc
int main(int argc, char **argv) {
  try {
    auto config = rmtc::config(argc, argv);
    rclcpp::init(argc, argv);
    {
      auto bot = std::make_shared<rmtc::Bot>(std::move(config));
      rclcpp::spin(bot);
    }
    if (rclcpp::ok())
      rclcpp::shutdown();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "RMTC startup failed: " << e.what() << std::endl;
    if (rclcpp::ok())
      rclcpp::shutdown();
    return 2;
  }
}
