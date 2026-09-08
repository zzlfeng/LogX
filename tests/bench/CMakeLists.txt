# T5.2 性能基线基准:独立 main(),非 gtest,不注册 ctest。
# 用法: cmake -DLOGX_BUILD_BENCH=ON .. && cmake --build . --target logx_bench
find_package(Threads REQUIRED)
add_executable(logx_bench
    logx_bench.cpp
)
target_link_libraries(logx_bench PRIVATE logx_core Threads::Threads)
