cmake_minimum_required(VERSION 3.20)

project(Perfetto LANGUAGES C CXX)

find_package(Threads)

add_library(perfetto STATIC src/perfetto.cc)

target_compile_features(perfetto PRIVATE cxx_std_23)

target_include_directories(perfetto PUBLIC include)

if (WIN32)
    # The perfetto library contains many symbols, so it needs the big object
    # format.
    if (MSVC)
        target_compile_options(perfetto PUBLIC "/bigobj")
    else ()
        target_compile_options(perfetto PUBLIC "-Wa,-mbig-obj")
        target_compile_options(perfetto PRIVATE "-Os")
    endif ()
    target_compile_definitions(perfetto PUBLIC -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    # On Windows we should link to WinSock2.
    target_link_libraries(perfetto PUBLIC ws2_32)
endif (WIN32)
