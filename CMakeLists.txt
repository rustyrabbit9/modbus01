cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME modbus01)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/modbus_slave.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Generate programmer-friendly images next to the ELF file.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMENT "Generating HEX/BIN images and printing firmware size"
)

# SEGGER J-Link targets shown directly in CLion's CMake target list.
include(cmake/jlink.cmake)
set(JLINK_DEVICE "STM32H750XB" CACHE STRING "SEGGER device name")
set(JLINK_INTERFACE "SWD" CACHE STRING "J-Link target interface")
set(JLINK_SPEED "4000" CACHE STRING "J-Link interface speed in kHz")
set(JLINK_SERIAL_NUMBER "" CACHE STRING
    "Optional J-Link serial number when more than one probe is connected")

if(JLINK_COMMANDER)
    set(_JLINK_USB_ARGUMENTS)
    if(JLINK_SERIAL_NUMBER)
        list(APPEND _JLINK_USB_ARGUMENTS -USB "${JLINK_SERIAL_NUMBER}")
    endif()

    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/jlink-probe.jlink" CONTENT
        "ExitOnError 1\nConnect\nExit\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/jlink-flash.jlink" CONTENT
        "ExitOnError 1\nReset\nHalt\nLoadFile \"$<TARGET_FILE:${CMAKE_PROJECT_NAME}>\"\nVerifyBin \"$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin\" 0x08000000\nReset\nGo\nExit\n")

    add_custom_target(probe
        COMMAND "${JLINK_COMMANDER}"
                -NoGui 1 -ExitOnError 1
                -Device "${JLINK_DEVICE}"
                -If "${JLINK_INTERFACE}" -Speed "${JLINK_SPEED}"
                -AutoConnect 1 ${_JLINK_USB_ARGUMENTS}
                -CommandFile "${CMAKE_BINARY_DIR}/jlink-probe.jlink"
        USES_TERMINAL
        COMMENT "Connecting to ${JLINK_DEVICE} through J-Link"
    )

    add_custom_target(flash
        COMMAND "${JLINK_COMMANDER}"
                -NoGui 1 -ExitOnError 1
                -Device "${JLINK_DEVICE}"
                -If "${JLINK_INTERFACE}" -Speed "${JLINK_SPEED}"
                -AutoConnect 1 ${_JLINK_USB_ARGUMENTS}
                -CommandFile "${CMAKE_BINARY_DIR}/jlink-flash.jlink"
        DEPENDS ${CMAKE_PROJECT_NAME}
        USES_TERMINAL
        COMMENT "Flashing and verifying ${CMAKE_PROJECT_NAME} through J-Link"
    )
else()
    message(WARNING
        "J-Link Commander was not found; the probe/flash targets are disabled. "
        "Install SEGGER J-Link or set JLINK_PATH.")
endif()
