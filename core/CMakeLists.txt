# heimdall_core: dependency-free engine core (C++ standard library only).
#
# Policy: this target must not link anything and its sources must not
# #include anything outside <...> (standard library / platform SDK) and
# <Heimdall/...>. Enforced by test/src/CorePolicySpec.cpp.
#
# Sources are listed explicitly (no GLOB) so every TU builds in parallel
# and new files require a CMake configure.

add_library(heimdall_core STATIC
  src/Arena.cpp
  src/MappedBuffer.cpp
  src/SyntaxTree.cpp
  src/Formatter.cpp
  src/Lexer.cpp
  src/LineTable.cpp
  src/GrammarParser.cpp
  src/ParseTree.cpp
  src/Preprocessor.cpp
  src/RuleEngine.cpp
  src/Completion.cpp
  src/Navigation.cpp
)
add_library(heimdall::core ALIAS heimdall_core)

target_include_directories(heimdall_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(heimdall_core PUBLIC cxx_std_26)

# STL-only PCH. Keeps <vector>/<string>/... out of the per-TU parse cost
# without pulling third-party or Heimdall headers into every unit.
target_precompile_headers(heimdall_core PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)

# No exceptions / no RTTI in the hot path: errors are std::expected and
# ErrorNode/Diagnostic, never throws. Cheap to enforce now, costly later.
target_compile_options(heimdall_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions;-fno-rtti>
  $<$<CXX_COMPILER_ID:MSVC>:/GR-;/EHs-c->
)
