# Benchmark executables for Phase A2-A4 features

# A2: SPSC Ring Buffer throughput
add_executable(bench_spsc bench-spsc-ring-buffer.cpp)
target_link_libraries(bench_spsc PRIVATE benchmark::benchmark Threads::Threads)

# A3: Object Pool vs heap allocation
add_executable(bench_pool bench-object-pool.cpp)
target_link_libraries(bench_pool PRIVATE benchmark::benchmark)

# A4: CRTP vs virtual dispatch
add_executable(bench_crtp bench-crtp-vs-virtual.cpp)
target_link_libraries(bench_crtp PRIVATE benchmark::benchmark)

# Put all benchmarks in bin/ directory
set_target_properties(bench_spsc bench_pool bench_crtp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
