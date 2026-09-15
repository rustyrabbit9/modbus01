# Locate SEGGER J-Link without requiring its installation directory in PATH.
# Override auto-detection with JLINK_PATH as a CMake cache or environment variable.

if(NOT JLINK_PATH AND DEFINED ENV{JLINK_PATH})
    file(TO_CMAKE_PATH "$ENV{JLINK_PATH}" JLINK_PATH)
endif()

if(CMAKE_HOST_WIN32 AND NOT JLINK_PATH)
    file(GLOB _JLINK_CANDIDATES LIST_DIRECTORIES TRUE
        "C:/Program Files/SEGGER/JLink*"
        "C:/Program Files (x86)/SEGGER/JLink*"
        "C:/SEGGER/JLink*")
    if(_JLINK_CANDIDATES)
        list(SORT _JLINK_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET _JLINK_CANDIDATES 0 JLINK_PATH)
    endif()
endif()

set(JLINK_PATH "${JLINK_PATH}" CACHE PATH "SEGGER J-Link installation directory")

find_program(JLINK_COMMANDER
    NAMES JLink JLink.exe JLinkExe
    HINTS "${JLINK_PATH}")
find_program(JLINK_GDB_SERVER
    NAMES JLinkGDBServerCL JLinkGDBServerCL.exe JLinkGDBServer
    HINTS "${JLINK_PATH}")
