# RMTC 机甲大师图灵对决赛参赛程序

队伍：**又猪又区队**；队员：**袁正航（单人）**。正式代码修订 **1.1.4**，默认策略 `balanced`。

使用C++17、ROS 2 Humble、Ubuntu22.04、x86_64。模型请求与主控制循环分线程；随机事件经LLM理解，本地加权Dijkstra和战术评估决定动作。实际生产逻辑、网关及JSON处理均为C++。

## 项目入口

- [技术报告正文与截图](docs/技术报告.md)：设计、实现、单人分工、AI协作、日程、问题与实验。
- [代码导航](代码导航.md)：模块职责与文件位置。
- [策略登记表](docs/策略登记表.md)：balanced / collect / combat / safe。
- [本次编译与回执证据](evidence/submission_20261031/)：153项检查、第一阶段40分及双向通信原始记录。

`src/`为实现，`include/`为接口，`tests/`为C++测试，`config/`为策略配置，`third_party/`为JSON库和许可证，`tools/`为启动辅助和离线验证工具，`launch/`为标准ROS启动描述。Python不参与机器人策略。

## 依赖与构建

已安装ROS2 Humble的Ubuntu22.04环境需有g++、CMake、libcurl开发库及std_msgs/rclcpp。JSON头文件随工程提供。

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev
cd RMTC
source /opt/ros/humble/setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install" -DBUILD_TESTING=ON
cmake --build build -j2
cmake --install build
ctest --test-dir build --output-on-failure -V
```

网页上传可能不保留脚本可执行权限，下面使用 `bash` 调用，避免依赖该权限。其他脚本若按 `./脚本` 运行，可先 `chmod +x build.sh run.sh run_container.sh tools/*.sh`。

## 第一阶段本机练习

先在主办方练习包目录的另一个终端运行：

```bash
ROS_DOMAIN_ID=73 RMTC_INTERNAL_DOMAIN_ID=74 bash start_referee.sh --stage 1 --port 18081
```

在本仓库目录运行：

```bash
ROS_DOMAIN_ID=73 bash run.sh --stage 1 --seat A --name "又猪又区队" --decision-file "$PWD/config/decision_config.json"
```

浏览器打开 http://127.0.0.1:18081，点击“开始 / 继续”。不能让参考车和本程序同时占用A席位。

## 第二阶段本机练习

停止上一阶段程序和裁判，练习包目录运行：

```bash
ROS_DOMAIN_ID=73 RMTC_INTERNAL_DOMAIN_ID=74 bash start_referee.sh --stage 2 --port 18081 --bots B,C,D
```

本仓库目录运行：

```bash
ROS_DOMAIN_ID=73 RMTC_TEAM_TOKEN=practice-A bash run.sh --stage 2 --seat A --name "又猪又区队" --gateway http://127.0.0.1:18081 --decision-file "$PWD/config/decision_config.json"
```

mock练习不证明真实模型理解或正式AI资格。完整第二阶段500Tick，第一阶段100Tick。

## 正式比赛接入

先在本地执行 `bash tools/set_token.sh`，隐藏输入本场令牌。再按现场分配填写三个变量（以下是占位说明，不能原样执行）：

```bash
export MATCH_DOMAIN='裁判分配的数字'
export MATCH_SEAT='本场A或B或C或D'
export MATCH_GATEWAY='裁判发放的完整网关地址'
ROS_DOMAIN_ID="$MATCH_DOMAIN" bash run.sh --stage 2 --seat "$MATCH_SEAT" --name "又猪又区队" --gateway "$MATCH_GATEWAY" --model deepseek-flash --max-tokens 512 --token-file "$PWD/.secrets/team.token" --decision-file "$PWD/config/decision_config.json"
```

第一阶段按现场域以 `--stage 1 --seat A` 启动，不要求AI调用。域、席位、队名、网关、模型及令牌均可配置；session_id和tick从当前感知读取，不能写死。每次换场须重新核对令牌和席位。

策略文件只含 `{"decision_type":"balanced"}`；正式人工切换须经裁判批准、登记，在允许窗口执行，每场最多两次。不要修改代码或添加坐标、动作参数来实时操控。

## 验证与限制

10月31日重新独立构建，5个CTest程序、153项命名检查全部通过；当前安装程序第一阶段实测40/40、0非法动作，101帧和100组动作回执核对通过。

第二阶段历史1.1.4四场练习得分A45、B55、C42.5、D49.6，均为mock陪练结果；D场伤害回归门槛仍未达到。真实模型语义、正式有线网络和主办方检录尚待验收。当前未显式发送thinking/response_format，不能宣称真实模型接入已验证。

代码借助AI辅助实现、排错与测试，单人负责规则核对、方案选择和材料提交。未采用以大幅降低B/C场为代价提升D场的实验候选。

源码、依赖与构建文件完整保留；历史的大量原始证据、镜像及编译产物未全部上传。`VERSION.json`中的镜像标识为本地交付记录，不代表本仓库附带镜像。v1.2不要求提交容器镜像。

不提交真实令牌、`.secrets/`、`.env`、`build/`或`install/`。语雀文档链接需在主办方提交入口另行提交。
