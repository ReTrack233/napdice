# 0004-local-jrrp 中取值逻辑的单元测试。DICE_SRC 指向打过补丁的 Dice! 源码中的 Dice 目录。
cmake_minimum_required(VERSION 3.16)
project(jrrp_test CXX)

set(DICE_SRC "" CACHE PATH "打过补丁的 Dice! 源码中的 Dice 目录")
if(NOT EXISTS "${DICE_SRC}/DiceJrrp.h")
  message(FATAL_ERROR "DiceJrrp.h not found under DICE_SRC='${DICE_SRC}'; apply 0004-local-jrrp.patch first")
endif()

add_executable(jrrp_test jrrp_test.cpp)
target_include_directories(jrrp_test PRIVATE "${DICE_SRC}")
set_target_properties(jrrp_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(jrrp_test PRIVATE /utf-8 /W4)
endif()
