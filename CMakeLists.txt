cmake_minimum_required(VERSION 3.16)
project(napdice VERSION 0.1.0 LANGUAGES CXX)

# dice-host: the driver executable. The Dice! core (w4123.Dice.dll) is built separately from
# https://github.com/Dice-Developer-Team/Dice — see .github/workflows/build-windows.yml.
#
# IMPORTANT (Windows): dice-host and the core exchange std::string / std::set across the DLL
# boundary, so both must be built with MSVC, in Release, with the static runtime (/MT), and for the
# same architecture.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(dice-host
  src/main.cpp
  src/platform.cpp
  src/log.cpp
  src/config.cpp
  src/ws_client.cpp
  src/onebot.cpp
  src/cq.cpp
  src/core.cpp
  src/driver.cpp
)
target_include_directories(dice-host PRIVATE src third_party third_party/dice)
target_compile_definitions(dice-host PRIVATE NAPDICE_VERSION="${PROJECT_VERSION}")

if(MSVC)
  set_property(TARGET dice-host PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(dice-host PRIVATE /utf-8 /W3 /EHsc /permissive- /Zc:__cplusplus)
  target_compile_definitions(dice-host PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  target_link_libraries(dice-host PRIVATE ws2_32 shell32)
elseif(WIN32)
  # MinGW: only useful for syntax checks; it cannot load an MSVC-built core.
  target_compile_definitions(dice-host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  target_link_libraries(dice-host PRIVATE ws2_32 shell32)
  target_link_options(dice-host PRIVATE -static)
else()
  find_package(Threads REQUIRED)
  target_compile_options(dice-host PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(dice-host PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

install(TARGETS dice-host RUNTIME DESTINATION .)

option(NAPDICE_BUILD_TESTS "Build unit tests" OFF)
if(NAPDICE_BUILD_TESTS)
  enable_testing()
  add_executable(napdice-unit tests/unit.cpp src/platform.cpp src/cq.cpp src/config.cpp)
  target_include_directories(napdice-unit PRIVATE src third_party)
  if(MSVC)
    set_property(TARGET napdice-unit PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(napdice-unit PRIVATE /utf-8 /EHsc /permissive-)
    target_compile_definitions(napdice-unit PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  elseif(WIN32)
    target_compile_definitions(napdice-unit PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_link_options(napdice-unit PRIVATE -static)
  else()
    target_link_libraries(napdice-unit PRIVATE ${CMAKE_DL_LIBS})
  endif()
  add_test(NAME unit COMMAND napdice-unit)
endif()
