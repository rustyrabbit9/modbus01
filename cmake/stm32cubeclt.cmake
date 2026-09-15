# Locate an STM32CubeCLT installation without requiring its tools to be in PATH.
# Override auto-detection with either the STM32_CUBE_CLT_PATH CMake cache variable
# or an environment variable with the same name.

if(NOT STM32_CUBE_CLT_PATH AND DEFINED ENV{STM32_CUBE_CLT_PATH})
    file(TO_CMAKE_PATH "$ENV{STM32_CUBE_CLT_PATH}" STM32_CUBE_CLT_PATH)
endif()

if(CMAKE_HOST_WIN32 AND NOT STM32_CUBE_CLT_PATH)
    file(GLOB _STM32_CUBE_CLT_CANDIDATES LIST_DIRECTORIES TRUE "C:/ST/STM32CubeCLT_*")
    if(_STM32_CUBE_CLT_CANDIDATES)
        list(SORT _STM32_CUBE_CLT_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET _STM32_CUBE_CLT_CANDIDATES 0 STM32_CUBE_CLT_PATH)
    endif()
endif()

set(STM32_CUBE_CLT_PATH "${STM32_CUBE_CLT_PATH}" CACHE PATH
    "STM32CubeCLT installation directory")

set(_STM32_GNU_HINTS)
set(_STM32_NINJA_HINTS)
if(STM32_CUBE_CLT_PATH)
    list(APPEND _STM32_GNU_HINTS "${STM32_CUBE_CLT_PATH}/GNU-tools-for-STM32/bin")
    list(APPEND _STM32_NINJA_HINTS "${STM32_CUBE_CLT_PATH}/Ninja/bin")
endif()
