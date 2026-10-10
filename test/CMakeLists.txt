FetchContent_Declare(
  gtest
  GIT_REPOSITORY "https://github.com/google/googletest"
  GIT_TAG        "v1.17.0"
)
FetchContent_MakeAvailable(gtest)

include(GoogleTest)

add_executable(Tests_run
  src/RefactoringSpec.cpp
  src/ProjectSymbolIndexSpec.cpp
  src/WorkspaceSpec.cpp
  src/SemanticDispatchSpec.cpp
  src/RuleSchedulerSpec.cpp
  src/PluginHostSpec.cpp
  src/CompileDatabaseSpec.cpp
  src/RuleConfigSpec.cpp
  src/CliInitSpec.cpp
  src/Cpp26FeaturesSpec.cpp
  src/CorePolicySpec.cpp
  src/ArchitecturePolicySpec.cpp
  src/SyntaxTreeSpec.cpp
  src/FormatterSpec.cpp
  src/LexerSpec.cpp
  src/ParseTreeSpec.cpp
  src/PreprocessorSpec.cpp
  src/RuleEngineSpec.cpp
  src/SemanticAnalyzerSpec.cpp
  src/SemanticModelSpec.cpp
  src/SemanticRulesSpec.cpp
  src/ApiRulesSpec.cpp
  src/DesignatedInitSpec.cpp
  src/DocumentationSpec.cpp
  src/SemanticModernizeSpec.cpp
  src/ModernizeCxxSpec.cpp
  src/SemanticBinderFlagsSpec.cpp
  src/SemanticTyperSpec.cpp
  src/SemanticFlowSpec.cpp
  src/CompletionSpec.cpp
  src/NavigationSpec.cpp
  src/SymbolOutlineSpec.cpp
  src/SymbolProtocolSpec.cpp
  src/WorkspaceSymbolIndexSpec.cpp
  src/WorkspaceFilesSpec.cpp
  src/DocumentSpec.cpp
  src/ThreadPoolSpec.cpp
  src/IncludeIndexSpec.cpp
  src/IncludeAnalyzerSpec.cpp
  src/ProjectRulesSpec.cpp
  src/ParserTruncationSpec.cpp
  src/IncrementalParseSpec.cpp
  src/FileDiscoverySpec.cpp
  src/CliOptionsSpec.cpp
  ../../lsp/Document.cpp
  ../../lsp/JsonRpc.cpp
  ../../lsp/SymbolProtocol.cpp
  ../../lsp/WorkspaceFiles.cpp
  ../../lsp/WorkspaceSymbolIndex.cpp
  ../../src/CliOptions.cpp
  ../../src/FileDiscovery.cpp
  ../../src/Init.cpp
)

target_include_directories(Tests_run PRIVATE ${PROJECT_SOURCE_DIR}/lsp ${PROJECT_SOURCE_DIR}/bench/src ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(Tests_run PRIVATE gtest gtest_main heimdall::refactor heimdall::config simdjson::simdjson)

target_compile_definitions(Tests_run PRIVATE HEIMDALL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_definitions(Tests_run PRIVATE HEIMDALL_BUILD_DIR="${PROJECT_BINARY_DIR}")
target_compile_definitions(Tests_run PRIVATE HEIMDALL_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")

target_precompile_headers(Tests_run PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)

gtest_discover_tests(Tests_run)

option(HEIMDALL_TEST_REFLECTION "Probe optional C++26 reflection support" ON)
set(HEIMDALL_REFLECTION_PROBE_AVAILABLE 0)
if (HEIMDALL_TEST_REFLECTION)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-std=c++26 -freflection")
  check_cxx_source_compiles([=[
#include <meta>
#if !defined(__cpp_impl_reflection) || __cpp_impl_reflection < 202506L
#error Reflection is unavailable
#endif
struct Sample { int value; };
static_assert(std::meta::identifier_of(^^Sample::value) == "value");
int main() { return 0; }
]=] HEIMDALL_CXX26_REFLECTION_AVAILABLE)
  unset(CMAKE_REQUIRED_FLAGS)

  if (HEIMDALL_CXX26_REFLECTION_AVAILABLE)
    set(HEIMDALL_REFLECTION_PROBE_AVAILABLE 1)
    add_executable(Cpp26ReflectionProbe src/ReflectionProbe.cpp)
    target_include_directories(Cpp26ReflectionProbe PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
    target_link_libraries(Cpp26ReflectionProbe PRIVATE gtest_main)
    target_compile_features(Cpp26ReflectionProbe PRIVATE cxx_std_26)
    target_compile_options(Cpp26ReflectionProbe PRIVATE -freflection)
    gtest_discover_tests(Cpp26ReflectionProbe)
    message(STATUS "C++26 reflection probe enabled (-freflection)")
  else ()
    message(STATUS "C++26 reflection probe unavailable with this compiler")
  endif ()
endif ()
target_compile_definitions(Tests_run PRIVATE
  HEIMDALL_REFLECTION_PROBE_AVAILABLE=${HEIMDALL_REFLECTION_PROBE_AVAILABLE})

add_library(CliSemanticFixture OBJECT EXCLUDE_FROM_ALL fixtures/semantic_unused.cpp)
target_compile_features(CliSemanticFixture PRIVATE cxx_std_26)
add_test(
  NAME CliSemanticDetectsUnusedLocal
  COMMAND ${CMAKE_COMMAND}
          "-DHEIMDALL_EXECUTABLE=$<TARGET_FILE:heimdall>"
          "-DCOMPILE_COMMANDS_FILE=${PROJECT_BINARY_DIR}/compile_commands.json"
          "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/semantic_unused.cpp"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckSemanticCli.cmake"
)

add_test(
  NAME CliParseReportsSyntaxErrors
  COMMAND ${CMAKE_COMMAND}
          "-DHEIMDALL_EXECUTABLE=$<TARGET_FILE:heimdall>"
          "-DGOOD_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/semantic_unused.cpp"
          "-DBAD_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/parse_syntax_error.cpp"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckParseCli.cmake"
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
  add_test(
    NAME LspProtocolSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspProtocolSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>" "${PROJECT_BINARY_DIR}/compile_commands.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/semantic_unused.cpp"
  )
  add_test(
    NAME LspStressSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspStressSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspNavigationSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspNavigationSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspSymbolSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspSymbolSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspRefactoringSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspRefactoringSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>" "${CMAKE_CXX_COMPILER}"
  )
  add_test(
    NAME LspUnversionedRefactoringSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspRefactoringSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>" "${CMAKE_CXX_COMPILER}" --unversioned
  )
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_test(
      NAME LspCliRenameSmoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspCliRenameSmoke.py"
              "$<TARGET_FILE:heimdall-lsp>" "${PROJECT_BINARY_DIR}/compile_commands.json"
              "${PROJECT_SOURCE_DIR}/src/cli.cpp"
    )
  endif ()
  add_test(
    NAME LspWorkspaceScanSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspWorkspaceScanSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspRuleConfigSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspRuleConfigSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspUnusedIncludeSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspUnusedIncludeSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspProjectRulesSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspProjectRulesSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspIncludeCompletionSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspIncludeCompletionSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspCancelSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspCancelSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspSnapshotConcurrencySmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspSnapshotConcurrencySmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspIncrementalSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspIncrementalSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>"
  )
  add_test(
    NAME LspMemberAccessSmoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LspMemberAccessSmoke.py"
            "$<TARGET_FILE:heimdall-lsp>" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
  )
endif ()
