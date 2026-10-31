/*
 * 文件：tests/ros_fault_probe.cpp
 * 作用：人工离线 ROS
 * 故障注入验收，给独立测试节点发送坏帧、重复帧、旧场次和断流。 订阅测试 A
 * 车动作，检查只响应可用的新感知、重复 Tick 使用相同动作、恢复后继续运行。
 * 必须在隔离 ROS_DOMAIN_ID 下使用；只在 BUILD_TESTING 编译，不安装进参赛镜像。
 * 用法：tools/verify_resilience.sh；本程序是合成输入测试，不能代替官方裁判或异机网络验收。
 */
#include "rmtc_bot/protocol.hpp"
#include <chrono>
#include <functional>
#include <iostream>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <thread>
using namespace rmtc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

Json sample(int tick, const std::string &session = "fault-s1") {
  return {{"tick", tick},
          {"session_id", session},
          {"vision_radius", 4},
          {"map", {{"width", 20}, {"height", 20}}},
          {"self",
           {{"team", "A"},
            {"position", {8, 8}},
            {"hp", 100},
            {"alive", true},
            {"protected", false},
            {"defending", false},
            {"last_action", nullptr}}},
          {"visible_entities", Json::array()},
          {"missions", Json::array()},
          {"score",
           {{"resource_value", 0},
            {"effective_damage", 0},
            {"invalid_actions", 0},
            {"mission_completed_ids", Json::array()}}},
          {"event_text", ""}};
}

class Probe : public rclcpp::Node {
  using Message = std_msgs::msg::String;
  rclcpp::Publisher<Message>::SharedPtr pub_;
  rclcpp::Subscription<Message>::SharedPtr sub_;
  std::vector<Json> actions_;
  int passed_ = 0;
  void pump(std::chrono::milliseconds duration) {
    const auto until = Clock::now() + duration;
    while (Clock::now() < until && rclcpp::ok()) {
      rclcpp::spin_some(get_node_base_interface());
      std::this_thread::sleep_for(5ms);
    }
  }
  void send(const std::string &data) {
    Message m;
    m.data = data;
    pub_->publish(m);
  }
  void pass(const std::string &name) {
    ++passed_;
    std::cout << "PASS " << name << std::endl;
  }
  void quiet(const std::string &data, const std::string &name,
             std::chrono::milliseconds duration = 250ms) {
    pump(80ms);
    const auto count = actions_.size();
    if (!data.empty())
      send(data);
    pump(duration);
    if (actions_.size() != count)
      throw Error(name + ": unexpected action");
    pass(name);
  }
  Json response(const Json &f) {
    pump(80ms);
    const auto count = actions_.size();
    send(f.dump());
    const auto until = Clock::now() + 2s;
    while (actions_.size() == count && Clock::now() < until)
      pump(10ms);
    if (actions_.size() != count + 1)
      throw Error("expected exactly one action");
    const Json a = actions_.back();
    if (a.at("session_id") != f.at("session_id") ||
        a.at("tick") != f.at("tick"))
      throw Error("action session/tick mismatch");
    const auto k = a.at("action").get<std::string>();
    if (k != "move" && k != "attack" && k != "defend" && k != "wait")
      throw Error("illegal action kind");
    return a;
  }

public:
  Probe() : Node("rmtc_offline_fault_probe") {
    const auto qos =
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile();
    pub_ = create_publisher<Message>("/team_A/perception", qos);
    sub_ = create_subscription<Message>(
        "/team_A/action", qos,
        [this](const Message &m) { actions_.push_back(strict_json(m.data)); });
  }
  void run() {
    const auto until = Clock::now() + 8s;
    while ((pub_->get_subscription_count() == 0 ||
            sub_->get_publisher_count() == 0) &&
           Clock::now() < until)
      pump(20ms);
    if (!pub_->get_subscription_count() || !sub_->get_publisher_count())
      throw Error("DDS discovery timed out");
    pass("bidirectional_discovery");
    const auto first = response(sample(5));
    pass("late_join_current_tick_only");
    const auto replay = response(sample(5));
    if (first != replay)
      throw Error("same tick action changed");
    pass("duplicate_tick_replays_identical_action");
    quiet(sample(4).dump(), "older_tick_ignored");
    quiet("{\"tick\":", "malformed_json_ignored");
    auto bad = sample(6);
    bad["self"]["team"] = "B";
    quiet(bad.dump(), "wrong_seat_ignored");
    bad = sample(6);
    bad["map"]["width"] = 21;
    quiet(bad.dump(), "same_session_map_change_ignored");
    response(sample(6));
    pass("valid_frame_recovers_after_bad_frames");
    quiet("", "silence_never_replays_stale_actions", 3500ms);
    response(sample(9));
    pass("stream_resumes_without_restart");
    response(sample(0, "fault-s2"));
    pass("new_session_resets_tick");
    quiet(sample(10).dump(), "retired_session_ignored");
    bad = sample(1, "fault-s2");
    bad["self"]["alive"] = false;
    bad["self"]["hp"] = 0;
    if (response(bad).at("action") != "wait")
      throw Error("dead robot did not wait");
    pass("dead_robot_waits");
    response(sample(2, "fault-s2"));
    pass("respawn_resumes");
    quiet(sample(500, "fault-s2").dump(), "final_state_has_no_action");
    Json previous = response(sample(100, "fault-receipts"));
    for (int tick = 101; tick <= 106; ++tick) {
      auto f = sample(tick, "fault-receipts");
      f["self"]["last_action"] = {{"tick", tick - 1},
          {"action", previous.at("action")}, {"moved", false},
          {"error", tick % 2 ? "E_BLOCKED" : "E_STATE"}};
      previous = response(f);
    }
    pass("error_receipts_keep_action_stream_live");
    std::cout << "RESULT " << passed_ << " passed, 0 failed" << std::endl;
  }
};
int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  try {
    auto probe = std::make_shared<Probe>();
    probe->run();
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    if (rclcpp::ok())
      rclcpp::shutdown();
    return 1;
  }
}
