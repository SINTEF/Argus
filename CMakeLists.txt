cmake_minimum_required(VERSION 3.10)
project(SensorPayload C)
set(PROJECT_AUTHOR "SINTEF Digital")
set(PROJECT_VERSION "0.1.0")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if (NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

get_filename_component(PSDK_C_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
set(PSDK_INFERRED_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if (PSDK_C_COMPILER_NAME MATCHES "^aarch64.*(gcc|clang)")
    set(PSDK_INFERRED_TARGET_PROCESSOR "aarch64")
elseif (PSDK_C_COMPILER_NAME MATCHES "^(arm.*gnueabihf|arm.*gnueabi).*(gcc|clang)")
    set(PSDK_INFERRED_TARGET_PROCESSOR "armv7")
elseif (PSDK_C_COMPILER_NAME MATCHES "^x86_64.*(gcc|clang)")
    set(PSDK_INFERRED_TARGET_PROCESSOR "x86_64")
endif()

set(PAYLOAD_SDK_DIR     "$ENV{PAYLOAD_SDK_DIR}" CACHE PATH "Path to Payload-SDK checkout")
set(PSDK_PLATFORM       "$ENV{PSDK_PLATFORM}" CACHE STRING "Payload SDK Linux platform sample folder")
set(PSDK_TARGET_OS      "$ENV{PSDK_TARGET_OS}" CACHE STRING "Payload SDK target OS (e.g. linux, rtos_freertos)")
set(PSDK_TOOLCHAIN_NAME "$ENV{PSDK_TOOLCHAIN_NAME}" CACHE STRING "Payload SDK static library toolchain folder")
set(PSDK_TARGET_PROCESSOR "$ENV{PSDK_TARGET_PROCESSOR}" CACHE STRING "Payload SDK target processor override")
set(ARGUS_DATA_DIR "/etc/argus" CACHE PATH "Default directory for sensor-fields.conf and en_big_screen/ on the target")

if (NOT PSDK_TARGET_PROCESSOR)
    set(PSDK_TARGET_PROCESSOR "${PSDK_INFERRED_TARGET_PROCESSOR}" CACHE STRING "Payload SDK target processor override" FORCE)
endif()


if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app_info.h")
    message(FATAL_ERROR "Missing src/app_info.h (DJI application credentials, not tracked in git). "
        "Copy src/app_info.h.example to src/app_info.h and fill it in.")
endif()

if (NOT PAYLOAD_SDK_DIR)
    message(FATAL_ERROR "Environment variable PAYLOAD_SDK_DIR not set. Please set it to the path of the Payload SDK.")
endif()
message(STATUS "Using Payload SDK from: ${PAYLOAD_SDK_DIR}")

if (NOT PSDK_PLATFORM)
    set(PSDK_PLATFORM "raspberry_pi" CACHE STRING "Payload SDK target platform" FORCE)
endif()
message(STATUS "Target platform: ${PSDK_PLATFORM}")

if (NOT PSDK_TARGET_OS)
    set(PSDK_TARGET_OS "linux" CACHE STRING "Payload SDK target OS" FORCE)
endif()

if (NOT PSDK_TOOLCHAIN_NAME)
    if (PSDK_TARGET_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(PSDK_TOOLCHAIN_NAME "x86_64-linux-gnu-gcc" CACHE STRING "Payload SDK static library toolchain folder" FORCE)
    elseif (PSDK_TARGET_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(PSDK_TOOLCHAIN_NAME "aarch64-linux-gnu-gcc" CACHE STRING "Payload SDK static library toolchain folder" FORCE)
    elseif (PSDK_TARGET_PROCESSOR MATCHES "^(armv7|armv7l|armhf|arm)$")
        set(PSDK_TOOLCHAIN_NAME "arm-linux-gnueabihf-gcc" CACHE STRING "Payload SDK static library toolchain folder" FORCE)
    else()
        message(FATAL_ERROR "Unable to infer PSDK_TOOLCHAIN_NAME for PSDK_TARGET_PROCESSOR='${PSDK_TARGET_PROCESSOR}'. Pass -DPSDK_TOOLCHAIN_NAME=<Payload-SDK lib folder>.")
    endif()
endif()
message(STATUS "SDK library toolchain: ${PSDK_TOOLCHAIN_NAME}")

set(PSDK_COMPILE_DEFINITIONS _GNU_SOURCE SYSTEM_ARCH_LINUX)
if (PSDK_TARGET_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    list(APPEND PSDK_COMPILE_DEFINITIONS PLATFORM_ARCH_x86_64=1)
elseif (PSDK_TARGET_PROCESSOR MATCHES "^(aarch64|arm64)$")
    list(APPEND PSDK_COMPILE_DEFINITIONS PLATFORM_ARCH_aarch64=1)
else()
    message(STATUS "No Payload SDK PLATFORM_ARCH_* macro defined for processor: ${PSDK_TARGET_PROCESSOR}")
endif()
message(STATUS "CMake system processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Payload SDK target processor: ${PSDK_TARGET_PROCESSOR}")
message(STATUS "Cross compiling: ${CMAKE_CROSSCOMPILING}")

# Make Payload SDK headers avaialble
set(PSDK_INCLUDE_DIR "${PAYLOAD_SDK_DIR}/psdk_lib/include")
set(PSDK_SAMPLE_DIR "${PAYLOAD_SDK_DIR}/samples/sample_c")
set(PSDK_MODULE_SAMPLE_DIR "${PSDK_SAMPLE_DIR}/module_sample")
set(PSDK_OSAL_INCLUDE_DIR "${PSDK_SAMPLE_DIR}/platform/${PSDK_TARGET_OS}/common")
set(PSDK_HEADER_PLATFORM "${PSDK_PLATFORM}")

set(PSDK_PLATFORM_INCLUDE_DIR "${PSDK_SAMPLE_DIR}/platform/${PSDK_TARGET_OS}/${PSDK_HEADER_PLATFORM}")
set(PSDK_LIB_DIR "${PAYLOAD_SDK_DIR}/psdk_lib/lib/${PSDK_TOOLCHAIN_NAME}")
set(PSDK_LIB "${PSDK_LIB_DIR}/libpayloadsdk.a")
set(LOCAL_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform/${PSDK_HEADER_PLATFORM}")
set(LOCAL_HAL_UART_SOURCE "${LOCAL_PLATFORM_DIR}/hal/hal_uart.c")
set(LOCAL_PPS_SOURCE "${LOCAL_PLATFORM_DIR}/application/pps.c")

message(STATUS "PSDK_INCLUDE_DIR: ${PSDK_INCLUDE_DIR}")
message(STATUS "PSDK_OSAL_INCLUDE_DIR: ${PSDK_OSAL_INCLUDE_DIR}")
message(STATUS "PSDK_PLATFORM_INCLUDE_DIR: ${PSDK_PLATFORM_INCLUDE_DIR}")
message(STATUS "LOCAL_PLATFORM_DIR: ${LOCAL_PLATFORM_DIR}")

set(COMMON_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PSDK_INCLUDE_DIR}
    ${PSDK_MODULE_SAMPLE_DIR}
    ${PSDK_OSAL_INCLUDE_DIR}
    ${LOCAL_PLATFORM_DIR}
    ${LOCAL_PLATFORM_DIR}/application
    ${PSDK_PLATFORM_INCLUDE_DIR}
    ${PSDK_PLATFORM_INCLUDE_DIR}/application
)

message(STATUS "PSDK_LIB: ${PSDK_LIB}")

if (NOT EXISTS "${PSDK_LIB}")
    message(FATAL_ERROR "Payload SDK static library not found: ${PSDK_LIB}")
endif()

file(GLOB_RECURSE PSDK_COMMON_SOURCES "${PSDK_OSAL_INCLUDE_DIR}/*.c")
file(GLOB_RECURSE PSDK_HAL_SOURCES "${PSDK_PLATFORM_INCLUDE_DIR}/hal/*.c")
if (EXISTS "${LOCAL_HAL_UART_SOURCE}")
    list(REMOVE_ITEM PSDK_HAL_SOURCES "${PSDK_PLATFORM_INCLUDE_DIR}/hal/hal_uart.c")
    list(APPEND PSDK_HAL_SOURCES "${LOCAL_HAL_UART_SOURCE}")
    message(STATUS "Using local UART HAL: ${LOCAL_HAL_UART_SOURCE}")
endif()

set(PSDK_MODULE_SAMPLE_SOURCES
    "${PSDK_MODULE_SAMPLE_DIR}/utils/cJSON.c"
    "${PSDK_MODULE_SAMPLE_DIR}/utils/util_buffer.c"
    "${PSDK_MODULE_SAMPLE_DIR}/utils/util_file.c"
    "${PSDK_MODULE_SAMPLE_DIR}/utils/util_link_list.c"
    "${PSDK_MODULE_SAMPLE_DIR}/utils/util_md5.c"
    "${PSDK_MODULE_SAMPLE_DIR}/utils/util_misc.c"
    "${PSDK_MODULE_SAMPLE_DIR}/utils/util_time.c"
)
if (EXISTS "${LOCAL_PPS_SOURCE}")
    set(PSDK_PLATFORM_APP_SOURCES "${LOCAL_PPS_SOURCE}")
    message(STATUS "Using local PPS implementation: ${LOCAL_PPS_SOURCE}")
else()
    set(PSDK_PLATFORM_APP_SOURCES "${PSDK_PLATFORM_INCLUDE_DIR}/application/pps.c")
endif()
set(PSDK_SOURCES
    ${PSDK_MODULE_SAMPLE_SOURCES}
    ${PSDK_COMMON_SOURCES}
    ${PSDK_HAL_SOURCES}
    ${PSDK_PLATFORM_APP_SOURCES}
)

set_source_files_properties(${PSDK_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-format;-Wno-format-truncation;-Wno-format-overflow"
)

add_executable(SensorPayload
    src/main.c
    src/sensor_reader.c
    src/payload_widget.c
    ${PSDK_SOURCES}
)

target_include_directories(SensorPayload PRIVATE ${COMMON_INCLUDE_DIRS})
target_compile_definitions(SensorPayload PRIVATE ${PSDK_COMPILE_DEFINITIONS} "ARGUS_DATA_DIR=\"${ARGUS_DATA_DIR}\"")

target_link_libraries(SensorPayload PRIVATE
    ${PSDK_LIB}
    m
    dl
    pthread
)
