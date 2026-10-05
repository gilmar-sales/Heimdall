# Project state and features depend downwards on semantic and syntax. Neither
# core nor semantic depends on this application-independent coordination layer.
add_library(heimdall_analysis STATIC
  src/Workspace.cpp
  src/AnalysisContext.cpp
  src/AnalysisFeatures.cpp
  src/RuleScheduler.cpp
  src/PluginHost.cpp
)
add_library(heimdall::analysis ALIAS heimdall_analysis)
target_include_directories(heimdall_analysis PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(heimdall_analysis PUBLIC cxx_std_26)
target_link_libraries(heimdall_analysis PUBLIC heimdall::semantic)
target_precompile_headers(heimdall_analysis PRIVATE ${PROJECT_SOURCE_DIR}/cmake/pch.hpp)
