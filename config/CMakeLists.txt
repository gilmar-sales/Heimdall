add_library(heimdall_config STATIC
  src/RuleConfig.cpp
)
add_library(heimdall::config ALIAS heimdall_config)

target_include_directories(heimdall_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(heimdall_config PUBLIC cxx_std_26)
target_link_libraries(heimdall_config PUBLIC heimdall::core PRIVATE simdjson::simdjson)

target_precompile_headers(heimdall_config PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)
