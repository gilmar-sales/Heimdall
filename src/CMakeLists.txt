# heimdall CLI: thin entry + explicit modules (no GLOB).
add_executable(heimdall
  cli.cpp
  CliOptions.cpp
  FileDiscovery.cpp
  Pipeline.cpp
  Reporting.cpp
)

target_link_libraries(heimdall PRIVATE heimdall::core heimdall::semantic heimdall::config)
target_compile_features(heimdall PRIVATE cxx_std_26)

target_precompile_headers(heimdall PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)
