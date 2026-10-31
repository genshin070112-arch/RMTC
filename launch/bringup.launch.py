# 文件：launch/bringup.launch.py
# 作用：ROS2 多参数启动描述
# 声明席位、阶段、网关等启动参数，启动本包 C++ 可执行程序 rmtc_node。
# 由 ros2 launch rmtc_bot bringup.launch.py 调用；此 Python 仅描述启动，不编写参赛决策。
# JSON 策略文件来自已安装包的 share/rmtc_bot/config/decision_config.json。

# ROS2 launch description only; the contestant and gateway are C++ executables.
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument('seat', default_value='A'),
        DeclareLaunchArgument('stage', description='必须明确选择阶段 1 或 2'),
        DeclareLaunchArgument('name', default_value='RMTC-CPP'),
        DeclareLaunchArgument('gateway', default_value='http://127.0.0.1:8080'),
        DeclareLaunchArgument('token_file', default_value=''),
        DeclareLaunchArgument('log_dir', default_value='logs'),
        Node(package='rmtc_bot', executable='rmtc_node', output='screen',
             arguments=['--seat', LaunchConfiguration('seat'), '--stage', LaunchConfiguration('stage'),
                        '--name', LaunchConfiguration('name'), '--gateway', LaunchConfiguration('gateway'),
                        '--token-file', LaunchConfiguration('token_file'), '--log-dir', LaunchConfiguration('log_dir'),
                        '--decision-file', PathJoinSubstitution([FindPackageShare('rmtc_bot'), 'config', 'decision_config.json'])])
    ])
