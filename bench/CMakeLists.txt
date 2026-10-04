set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)

FetchContent_Declare(
  google-benchmark
  GIT_REPOSITORY "https://github.com/google/benchmark.git"
  GIT_TAG "v1.9.5"
)
FetchContent_MakeAvailable(google-benchmark)

add_executable(CoreBench
  src/ArenaBench.cpp
  src/MappedBufferBench.cpp
  src/SyntaxTreeBench.cpp
  src/FormatterBench.cpp
  src/LexerBench.cpp
  src/PreprocessorBench.cpp
  src/RuleEngineBench.cpp
  src/ParserBench.cpp
  src/CompletionBench.cpp
)

target_link_libraries(CoreBench PRIVATE heimdall::core benchmark::benchmark benchmark::benchmark_main)
target_compile_definitions(CoreBench PRIVATE HEIMDALL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_features(CoreBench PRIVATE cxx_std_26)

target_precompile_headers(CoreBench PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)
