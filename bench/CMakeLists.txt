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

add_executable(SemanticBench src/SemanticBench.cpp)
target_link_libraries(SemanticBench PRIVATE heimdall::core heimdall::semantic benchmark::benchmark benchmark::benchmark_main)
target_compile_definitions(SemanticBench PRIVATE HEIMDALL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_features(SemanticBench PRIVATE cxx_std_26)

add_executable(WorkspaceBench src/WorkspaceBench.cpp)
target_link_libraries(WorkspaceBench PRIVATE heimdall::analysis benchmark::benchmark benchmark::benchmark_main)
target_compile_features(WorkspaceBench PRIVATE cxx_std_26)

# Review section 7 validation tools. Plain executables (not Google Benchmark): they
# report percentiles / pass-fail limits, which the micro-benchmark harness does not.
# Corpus: the stb single headers next to this file (src/StbCorpus.hpp).
set(HEIMDALL_STB_DEFINITION HEIMDALL_STB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

# End-to-end LSP latency (didChange + completion) and server memory per document.
# Drives the real heimdall-lsp binary over stdio.
add_executable(LspLatencyBench src/LspLatencyBench.cpp)
add_dependencies(LspLatencyBench heimdall-lsp)
target_compile_definitions(LspLatencyBench PRIVATE ${HEIMDALL_STB_DEFINITION}
  HEIMDALL_LSP_EXE="$<TARGET_FILE:heimdall-lsp>")
target_compile_features(LspLatencyBench PRIVATE cxx_std_26)
if (WIN32)
  target_link_libraries(LspLatencyBench PRIVATE psapi)
else ()
  find_package(Threads REQUIRED)
  target_link_libraries(LspLatencyBench PRIVATE Threads::Threads)
endif ()

# Outline extraction and workspace symbol index (Ctrl+T) cost: extraction over a 20k-line
# document, conversion to LSP positions and query latency over synthetic workspaces.
add_executable(SymbolBench
  src/SymbolBench.cpp
  ${PROJECT_SOURCE_DIR}/lsp/Document.cpp
  ${PROJECT_SOURCE_DIR}/lsp/WorkspaceSymbolIndex.cpp
)
target_include_directories(SymbolBench PRIVATE ${PROJECT_SOURCE_DIR}/lsp)
target_link_libraries(SymbolBench PRIVATE heimdall::core benchmark::benchmark benchmark::benchmark_main)
target_compile_definitions(SymbolBench PRIVATE ${HEIMDALL_STB_DEFINITION})
target_compile_features(SymbolBench PRIVATE cxx_std_26)

# In-process parse tree footprint (bytes/line, bytes/token).
add_executable(DocumentMemoryBench src/DocumentMemoryBench.cpp)
target_link_libraries(DocumentMemoryBench PRIVATE heimdall::core)
target_compile_definitions(DocumentMemoryBench PRIVATE ${HEIMDALL_STB_DEFINITION})
target_compile_features(DocumentMemoryBench PRIVATE cxx_std_26)
if (WIN32)
  target_link_libraries(DocumentMemoryBench PRIVATE psapi)
endif ()

# Parser robustness under truncation (incomplete typing): hang / time / error bounds.
add_executable(ParserTruncationFuzz src/ParserTruncationFuzz.cpp)
target_link_libraries(ParserTruncationFuzz PRIVATE heimdall::core)
target_compile_definitions(ParserTruncationFuzz PRIVATE ${HEIMDALL_STB_DEFINITION})
target_compile_features(ParserTruncationFuzz PRIVATE cxx_std_26)
if (NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(ParserTruncationFuzz PRIVATE Threads::Threads)
endif ()
