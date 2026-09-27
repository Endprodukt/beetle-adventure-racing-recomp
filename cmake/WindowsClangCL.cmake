# Windows host toolchain for Beetle Adventure Racing Recomp.
# Keep every Windows entry point (CMake presets and Visual Studio legacy CMake settings)
# on the same supported compiler/generator pair, without requiring a special developer shell.
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
        "Unsupported Windows generator '${CMAKE_GENERATOR}'. Beetle Adventure Racing Recomp requires Ninja with clang-cl.")
endif()

set(_BAR_CLANG_CL "")
set(_BAR_NINJA "")
set(_BAR_VS_INSTALL "")

# 1) Respect tools already available in PATH.
find_program(_BAR_CLANG_CL_FROM_PATH NAMES clang-cl clang-cl.exe)
if(_BAR_CLANG_CL_FROM_PATH)
    set(_BAR_CLANG_CL "${_BAR_CLANG_CL_FROM_PATH}")
endif()
find_program(_BAR_NINJA_FROM_PATH NAMES ninja ninja.exe)
if(_BAR_NINJA_FROM_PATH)
    set(_BAR_NINJA "${_BAR_NINJA_FROM_PATH}")
endif()

# 2) Locate the latest Visual Studio / Build Tools installation with vswhere.
# Use SystemDrive plus a literal Program Files (x86) path; CMake cannot reference an
# environment variable named ProgramFiles(x86) directly.
set(_BAR_VSWHERE "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
if(EXISTS "${_BAR_VSWHERE}")
    execute_process(
        COMMAND "${_BAR_VSWHERE}" -latest -products * -property installationPath
        OUTPUT_VARIABLE _BAR_VS_INSTALL
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

# Visual Studio ships Ninja with its CMake integration and clang-cl with the optional
# C++ Clang tools for Windows component.
if(_BAR_VS_INSTALL)
    if(NOT _BAR_CLANG_CL)
        foreach(_BAR_LLVM_SUBDIR
                "VC/Tools/Llvm/x64/bin/clang-cl.exe"
                "VC/Tools/Llvm/bin/clang-cl.exe")
            if(EXISTS "${_BAR_VS_INSTALL}/${_BAR_LLVM_SUBDIR}")
                set(_BAR_CLANG_CL "${_BAR_VS_INSTALL}/${_BAR_LLVM_SUBDIR}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT _BAR_NINJA)
        foreach(_BAR_NINJA_SUBDIR
                "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
                "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja")
            if(EXISTS "${_BAR_VS_INSTALL}/${_BAR_NINJA_SUBDIR}")
                set(_BAR_NINJA "${_BAR_VS_INSTALL}/${_BAR_NINJA_SUBDIR}")
                break()
            endif()
        endforeach()
    endif()
endif()

# 3) Fallbacks for standalone LLVM/Ninja and VS installations that vswhere cannot classify.
if(NOT _BAR_CLANG_CL)
    file(GLOB _BAR_CLANG_CL_CANDIDATES
        "$ENV{ProgramFiles}/LLVM/bin/clang-cl.exe"
        "$ENV{SystemDrive}/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/x64/bin/clang-cl.exe"
        "$ENV{SystemDrive}/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/bin/clang-cl.exe"
        "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/x64/bin/clang-cl.exe"
        "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/bin/clang-cl.exe"
    )
    if(_BAR_CLANG_CL_CANDIDATES)
        list(GET _BAR_CLANG_CL_CANDIDATES 0 _BAR_CLANG_CL)
    endif()
endif()

if(NOT _BAR_NINJA)
    file(GLOB _BAR_NINJA_CANDIDATES
        "$ENV{ProgramFiles}/Ninja/ninja.exe"
        "$ENV{SystemDrive}/Program Files/Microsoft Visual Studio/2022/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
        "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/2022/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
    )
    if(_BAR_NINJA_CANDIDATES)
        list(GET _BAR_NINJA_CANDIDATES 0 _BAR_NINJA)
    endif()
endif()

if(NOT _BAR_CLANG_CL OR NOT EXISTS "${_BAR_CLANG_CL}")
    message(FATAL_ERROR
        "clang-cl.exe was not found. Install Visual Studio 2022 / Build Tools component "
        "'C++ Clang tools for Windows' (Microsoft.VisualStudio.Component.VC.Llvm.Clang), "
        "or install standalone LLVM.")
endif()

if(NOT _BAR_NINJA OR NOT EXISTS "${_BAR_NINJA}")
    message(FATAL_ERROR
        "ninja.exe was not found. Install Visual Studio 2022 CMake tools or Ninja. "
        "The normal Visual Studio CMake integration includes Ninja.")
endif()

message(STATUS "BAR Windows compiler: ${_BAR_CLANG_CL}")
message(STATUS "BAR Windows Ninja: ${_BAR_NINJA}")
set(CMAKE_C_COMPILER "${_BAR_CLANG_CL}" CACHE FILEPATH "Windows C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${_BAR_CLANG_CL}" CACHE FILEPATH "Windows C++ compiler" FORCE)
set(CMAKE_MAKE_PROGRAM "${_BAR_NINJA}" CACHE FILEPATH "Ninja executable" FORCE)
