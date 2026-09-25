# cherrydap_s31_wifi —— ESP32-S31 **无线** CMSIS-DAP 探针（WiFi + TCP）
#
# 链路：OpenOCD ──TCP──> ESP32-S31 ──SWD(17~22MHz)──> 目标（本机实测用 STM32F103）
# SWD 位时序层整套复用 cherrydap_s31_fast（dedicated GPIO + 无分支展开汇编），
# 本工程只把**传输层**从 USB bulk 换成 WiFi 上的 TCP。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 最小构建：只编 main + 它依赖的组件，编得快、固件小
idf_build_set_property(MINIMAL_BUILD ON)
project(cherrydap_s31_wifi)
