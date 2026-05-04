cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

project(mktorrent
  VERSION 1.0
  LANGUAGES CXX)

add_executable(mktorrent main.cpp)
target_compile_features(mktorrent PRIVATE cxx_std_17)

find_package(LibtorrentRasterbar CONFIG QUIET)

if(TARGET LibtorrentRasterbar::torrent-rasterbar)
  target_link_libraries(mktorrent PRIVATE LibtorrentRasterbar::torrent-rasterbar)
else(TARGET torrent-rasterbar::torrent-rasterbar)
  target_link_libraries(mktorrent PRIVATE torrent-rasterbar::torrent-rasterbar)
endif()

if(MSVC)
  if((DEFINED VCPKG_CRT_LINKAGE AND VCPKG_CRT_LINKAGE STREQUAL "static")
      OR (DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static$"))
    set_property(TARGET mktorrent PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  target_compile_options(mktorrent PRIVATE /W4 /permissive-)
  target_compile_definitions(mktorrent PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  target_compile_options(mktorrent PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mktorrent RUNTIME DESTINATION bin)

