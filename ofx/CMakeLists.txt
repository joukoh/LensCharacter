# LensCharacter OpenFX plugin — Windows build (CUDA for NVIDIA GPUs, CPU otherwise).
# macOS builds with the Makefile instead.
#
#   build-windows.bat                 configure, build, and run the self-test (see WINDOWS.md)
#
# or by hand, from a "Developer PowerShell for VS 2022":
#   cmake -S . -B build-win -A x64
#   cmake --build build-win --config Release
#   cmake --install build-win          (as administrator; Resolve closed)
#
# Options:
#   -DOFX_SDK=<path>                    Resolve's OpenFX folder (default: found where Resolve installs it)
#   -DLC_CUDA=OFF                       build without CUDA (Resolve then renders this effect on the CPU)
#   -DCMAKE_CUDA_ARCHITECTURES=...      GPU generations to compile for (default: 75;86;89 plus PTX)

cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
    message(FATAL_ERROR "CMakeLists.txt builds the Windows plugin. On macOS use the Makefile (make, make test).")
endif()

# Turing (RTX 20), Ampere (RTX 30), Ada (RTX 40) as machine code, plus Ada PTX that newer GPUs
# (RTX 50 and later) compile when the plugin first loads.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "75-real;86-real;89-real;89-virtual")
endif()

project(LensCharacter VERSION 0.4 LANGUAGES CXX)

# Resolve 21 installs the SDK under ProgramData; earlier versions put it next to the program
string(REPLACE "\\" "/" _programData "$ENV{ProgramData}")
set(_sdkCandidates
    "${_programData}/Blackmagic Design/DaVinci Resolve/Support/Developer/OpenFX"
    "C:/Program Files/Blackmagic Design/DaVinci Resolve/Developer/OpenFX")
if(NOT OFX_SDK)
    foreach(_c IN LISTS _sdkCandidates)
        if(EXISTS "${_c}/Support/Library/ofxsCore.cpp")
            set(_sdkFound "${_c}")
            break()
        endif()
    endforeach()
    # FORCE: an earlier configure that found nothing leaves an empty value in the cache
    set(OFX_SDK "${_sdkFound}" CACHE PATH "DaVinci Resolve's OpenFX SDK folder" FORCE)
endif()
set(OFX_PLUGIN_DIR "C:/Program Files/Common Files/OFX/Plugins" CACHE PATH "Where `cmake --install` puts the plugin")
option(LC_CUDA "Build the CUDA render path (needs the CUDA Toolkit)" ON)

if(NOT EXISTS "${OFX_SDK}/Support/Library/ofxsCore.cpp")
    list(JOIN _sdkCandidates "\n  " _sdkList)
    message(FATAL_ERROR "OpenFX SDK not found (OFX_SDK=\"${OFX_SDK}\"). Resolve installs it in one of:\n  ${_sdkList}\n"
                        "Install DaVinci Resolve, or pass -DOFX_SDK=<path to its Developer/OpenFX folder>.")
endif()
message(STATUS "LensCharacter: OpenFX SDK ${OFX_SDK}")

set(LC_HAVE_CUDA OFF)
if(LC_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(LC_HAVE_CUDA ON)
    else()
        message(WARNING "CUDA Toolkit not found: building without the CUDA render path. Install the CUDA Toolkit "
                        "(with its Visual Studio integration) and delete the build folder to enable it.")
    endif()
endif()
message(STATUS "LensCharacter: CUDA render path ${LC_HAVE_CUDA}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SUP ${OFX_SDK}/Support/Library)

# Code shared by the plugin and the self-test
add_library(lc_core STATIC
    ${SRC}/Mixer.cpp
    ${SRC}/Platform.cpp
    ${SRC}/Profiles.cpp
    ${SRC}/LensBrowserModel.cpp)
if(LC_HAVE_CUDA)
    target_sources(lc_core PRIVATE ${SRC}/CudaKernel.cu)
    target_compile_definitions(lc_core PUBLIC LC_HAVE_CUDA=1)
    # The static CUDA runtime: no CUDA DLL to ship; the NVIDIA driver is all a user needs
    find_package(CUDAToolkit REQUIRED)
    set_target_properties(lc_core PROPERTIES CUDA_RUNTIME_LIBRARY Static)
    target_link_libraries(lc_core PUBLIC CUDA::cudart_static)
    # cudart_static.lib asks for the static C runtime (LIBCMT); everything else uses the DLL runtime
    # (/MD, CMake's default), so ignore that request rather than link two runtimes (LNK4098).
    # The DLL runtime it is: the static one made the CPU fallback ~35% slower, and Resolve installs
    # the Visual C++ runtime anyway. Anything cudart truly needed from LIBCMT would fail to link.
    target_link_options(lc_core INTERFACE /NODEFAULTLIB:LIBCMT)
endif()
target_include_directories(lc_core PUBLIC ${SRC})
target_compile_definitions(lc_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
# Sources are UTF-8 (lens names, "…", "·"); without /utf-8 MSVC reads them in the ANSI code page
target_compile_options(lc_core PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /W3 /wd4244 /wd4267 /wd4305>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8 -Xcompiler=/wd4244>)
set_target_properties(lc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The plugin: an OpenFX bundle around LensCharacter.ofx (a DLL)
add_library(LensCharacter MODULE
    ${SRC}/LensCharacterPlugin.cpp
    ${SRC}/LensBrowserWin.cpp
    ${SUP}/ofxsCore.cpp
    ${SUP}/ofxsImageEffect.cpp
    ${SUP}/ofxsInteract.cpp
    ${SUP}/ofxsLog.cpp
    ${SUP}/ofxsMultiThread.cpp
    ${SUP}/ofxsParams.cpp
    ${SUP}/ofxsProperty.cpp
    ${SUP}/ofxsPropertyValidation.cpp)
target_include_directories(LensCharacter PRIVATE ${OFX_SDK}/OpenFX-1.4/include ${OFX_SDK}/Support/include)
target_link_libraries(LensCharacter PRIVATE lc_core comctl32 user32 gdi32)
target_compile_options(LensCharacter PRIVATE /wd4996)   # the SDK's own sprintf/strcpy use
set_target_properties(LensCharacter PROPERTIES PREFIX "" SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<1:bin>")

set(BUNDLE "${CMAKE_BINARY_DIR}/LensCharacter.ofx.bundle")
file(GLOB PROFILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/profiles/*.json")
add_custom_command(TARGET LensCharacter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE}/Contents/Win64" "${BUNDLE}/Contents/Resources/profiles"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:LensCharacter>" "${BUNDLE}/Contents/Win64/LensCharacter.ofx"
    COMMAND ${CMAKE_COMMAND} -E copy ${PROFILES} "${BUNDLE}/Contents/Resources/profiles/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/resources/ATTRIBUTION.txt" "${BUNDLE}/Contents/Resources/"
    COMMENT "Assembling LensCharacter.ofx.bundle")

# Self-test: library, mixer, CPU kernel, and CPU vs CUDA
add_executable(selftest tests/selftest.cpp)
target_link_libraries(selftest PRIVATE lc_core)
set_target_properties(selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<1:bin>")
add_dependencies(selftest LensCharacter)   # it reads the profiles from the bundle

enable_testing()
add_test(NAME selftest COMMAND selftest "${CMAKE_BINARY_DIR}")
set_tests_properties(selftest PROPERTIES ENVIRONMENT "LENSCHARACTER_PROFILES=${BUNDLE}/Contents/Resources/profiles")

install(DIRECTORY "${BUNDLE}" DESTINATION "${OFX_PLUGIN_DIR}")
