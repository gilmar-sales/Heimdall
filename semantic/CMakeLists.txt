# heimdall_semantic: compile database, local type oracle, binder/typer and
# the project layer (HeaderSummary, ProjectIndex).
# Depends on heimdall::core. simdjson is PRIVATE:
# it must not leak through public headers (CompileDatabase.hpp and
# SemanticAnalyzer.hpp are STL + Heimdall only).

add_library(heimdall_semantic STATIC
  src/SourceOverlay.cpp
  src/CompileDatabase.cpp
  src/SemanticAnalyzer.cpp
  src/IncludeIndex.cpp
  src/IncludeAnalyzer.cpp
  src/SemanticModel.cpp
  src/Binder.cpp
  src/Typer.cpp
  src/SemanticRules.cpp
  src/HeaderSummary.cpp
  src/ProjectIndex.cpp
  src/IncludeWhatYouUse.cpp
  src/ModernizeFinal.cpp
  src/Flow.cpp
  src/ConstantAnalysis.cpp
  src/ModernizeConst.cpp
  src/ApiRules.cpp
  src/DesignatedInit.cpp
  src/Documentation.cpp
)
add_library(heimdall::semantic ALIAS heimdall_semantic)

target_include_directories(heimdall_semantic PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(heimdall_semantic PUBLIC cxx_std_26)

target_link_libraries(heimdall_semantic
  PUBLIC heimdall::core
  PRIVATE simdjson::simdjson
)

target_precompile_headers(heimdall_semantic PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)
