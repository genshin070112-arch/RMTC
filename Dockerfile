# 文件：Dockerfile
# 作用：参赛镜像构建说明
# 第一阶段编译并运行 C++ 测试；第二阶段复制安装产物，以非 root 用户运行程序。
# 用法：docker build -t rmtc-cpp:1.1 .；运行见 run_container.sh；镜像中不存比赛令牌。

FROM ros:humble-ros-base-jammy@sha256:566b87e6dfc48392189a9c32e594e84060fa9948de93b04e536f39475a1f6963 AS builder
SHELL ["/bin/bash", "-c"]
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /buildsrc
COPY CMakeLists.txt package.xml ./
COPY include include
COPY third_party third_party
COPY src src
COPY tests tests
COPY config config
COPY launch launch
RUN source /opt/ros/humble/setup.bash && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/rmtc -DBUILD_TESTING=ON && cmake --build build -j2 && ctest --test-dir build --output-on-failure && cmake --install build
FROM ros:humble-ros-base-jammy@sha256:566b87e6dfc48392189a9c32e594e84060fa9948de93b04e536f39475a1f6963
SHELL ["/bin/bash", "-c"]
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4 && rm -rf /var/lib/apt/lists/* && useradd --uid 1000 --create-home contestant
COPY --from=builder /opt/rmtc /opt/rmtc
WORKDIR /app
COPY config /app/config
COPY tools/container_entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh && mkdir -p /app/logs /app/.secrets && chown -R contestant:contestant /app
USER contestant
ENV ROS_LOCALHOST_ONLY=0 RMW_IMPLEMENTATION=rmw_fastrtps_cpp
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["--help"]
