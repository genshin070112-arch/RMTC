/*
 * 文件：tests/referee_probe.cpp
 * 作用：独立练习裁判的人工验收夹具，用预设动作对照 v1.1 结算。
 * 输入离线计划 JSON（测试席位、动作区间、总 Tick）；订阅这些测试车感知并保存
 * JSONL。 仅 BUILD_TESTING
 * 构建，不安装进参赛程序。不能在正式比赛使用此多席位夹具。 不调用
 * Planner，不向参赛策略提供裁判管理/全图信息；计划由独立验收脚本生成。
 */
#include "rmtc_bot/protocol.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
using namespace rmtc;
class Probe : public rclcpp::Node {
  using Message = std_msgs::msg::String;
  Json plan_;
  std::ofstream log_;
  std::map<std::string, rclcpp::Publisher<Message>::SharedPtr> publishers_;
  std::vector<rclcpp::Subscription<Message>::SharedPtr> subscriptions_;
  std::set<std::string> done_;

public:
  Probe(Json plan, const std::string &output)
      : Node("rmtc_offline_referee_probe"), plan_(std::move(plan)),
        log_(output) {
    if (!log_)
      throw Error("probe log open failed");
    auto qos =
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile();
    for (const auto &[seat, actions] : plan_.at("actions").items()) {
      (void)actions;
      publishers_[seat] =
          create_publisher<Message>("/team_" + seat + "/action", qos);
      subscriptions_.push_back(create_subscription<Message>(
          "/team_" + seat + "/perception", qos,
          [this, seat](const Message &message) {
            try {
              const auto f = parse_frame(message.data, seat);
              if (f.session != plan_.at("session_id").get<std::string>())
                return;
              if (f.tick >= plan_.at("max_ticks").get<int>()) {
                log_ << Json({{"seat", seat},
                              {"perception", f.raw},
                              {"action", nullptr}})
                            .dump()
                     << std::endl;
                done_.insert(seat);
                if (done_.size() == publishers_.size())
                  rclcpp::shutdown();
                return;
              }
              std::string a = "wait", dir;
              for (const auto &rule : plan_["actions"][seat])
                if (rule.at("from").get<int>() <= f.tick &&
                    f.tick < rule.at("to").get<int>()) {
                  a = rule.at("action").get<std::string>();
                  dir = rule.value("direction", "");
                  break;
                }
              auto out = action(f, a, dir, 2);
              Message m;
              m.data = out.dump();
              publishers_.at(seat)->publish(m);
              log_ << Json({{"seat", seat},
                            {"perception", f.raw},
                            {"action", out}})
                          .dump()
                   << std::endl;
            } catch (const std::exception &e) {
              std::cerr << "probe error: " << e.what() << std::endl;
              rclcpp::shutdown();
            }
          }));
    }
    std::cout << "PROBE_READY" << std::endl;
  }
};
int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: referee_probe plan.json output.jsonl\n";
    return 2;
  }
  try {
    std::ifstream file(argv[1]);
    std::string data((std::istreambuf_iterator<char>(file)), {});
    auto plan = strict_json(data);
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<Probe>(plan, argv[2]));
    if (rclcpp::ok())
      rclcpp::shutdown();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
