add_library(heimdall_refactor STATIC
  src/RefactoringPlan.cpp
  src/Rename.cpp
  src/ExtractVariable.cpp
  src/MacroSafety.cpp
)
add_library(heimdall::refactor ALIAS heimdall_refactor)
target_include_directories(heimdall_refactor PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(heimdall_refactor PUBLIC cxx_std_26)
target_link_libraries(heimdall_refactor PUBLIC heimdall::analysis)
target_precompile_headers(heimdall_refactor PRIVATE ${PROJECT_SOURCE_DIR}/cmake/pch.hpp)
