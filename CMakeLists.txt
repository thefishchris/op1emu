cmake_minimum_required(VERSION 3.20)

project(op1emu)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

# Fetch spdlog
message(STATUS "Fetching spdlog...")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.11.0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(spdlog)
message(STATUS "spdlog fetched successfully")

# Fetch GLFW
message(STATUS "Fetching GLFW...")
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL " " FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL " " FORCE)
set(GLFW_INSTALL OFF CACHE BOOL " " FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glfw)
message(STATUS "GLFW fetched successfully")
find_package(OpenGL REQUIRED)

# Fetch nlohmann/json
message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)
message(STATUS "nlohmann/json fetched successfully")

# Fetch stb (for image loading)
message(STATUS "Fetching stb...")
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(stb)
message(STATUS "stb fetched successfully")

# Fetch cqueue
message(STATUS "Fetching cqueue...")
FetchContent_Declare(cqueue
  GIT_REPOSITORY https://github.com/torrentg/cqueue.git
  GIT_TAG        1.0.6
)
FetchContent_MakeAvailable(cqueue)
message(STATUS "cqueue fetched successfully")
add_library(cqueue INTERFACE)
target_include_directories(cqueue INTERFACE ${cqueue_SOURCE_DIR})

# Fetch uvw
message(STATUS "Fetching uvw...")
FetchContent_Declare(
    uvw
    GIT_REPOSITORY https://github.com/skypjack/uvw.git
    GIT_TAG        v3.4.0_libuv_v1.48
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BUILD_UVW_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(uvw)
message(STATUS "uvw fetched successfully")

# Fetch miniaudio (single-header audio library)
message(STATUS "Fetching miniaudio...")
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniaudio)
message(STATUS "miniaudio fetched successfully")

find_package(Threads REQUIRED)

add_subdirectory(ext/bcore EXCLUDE_FROM_ALL)

file(GLOB SOURCES ext/cqueue/*.cpp)
add_library(ext ${SOURCES})
# use C++20 for ext
set_target_properties(ext PROPERTIES CXX_STANDARD 20)
target_include_directories(ext PUBLIC ext)
target_link_libraries(ext PRIVATE cqueue)

file(GLOB_RECURSE SOURCES src/*.cpp)
add_library(emulator ${SOURCES})
target_include_directories(emulator PUBLIC src)
target_include_directories(emulator PRIVATE ext)
target_link_libraries(emulator PRIVATE bfin-core spdlog::spdlog)

add_executable(ldrdump tools/ldrdump.cpp)
target_link_libraries(ldrdump emulator)

file(GLOB_RECURSE SOURCES gui/*.cpp)
add_executable(op1emu ${SOURCES})
target_link_libraries(op1emu PRIVATE emulator glfw OpenGL::GL nlohmann_json::nlohmann_json uvw ext Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(op1emu PRIVATE ${stb_SOURCE_DIR} ${miniaudio_SOURCE_DIR})

add_executable(op1emu-headless tools/headless_main.cpp gui/headless.cpp gui/headless_run.cpp)
target_include_directories(op1emu-headless PRIVATE gui)
target_link_libraries(op1emu-headless PRIVATE emulator nlohmann_json::nlohmann_json Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(headless_tests tests/headless_tests.cpp gui/headless.cpp)
    target_include_directories(headless_tests PRIVATE gui)
    target_link_libraries(headless_tests PRIVATE emulator nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME headless_tests COMMAND headless_tests ${CMAKE_CURRENT_SOURCE_DIR}/gui/ui.json)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME headless_cli_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_cli_tests.py
            $<TARGET_FILE:op1emu-headless> ${CMAKE_CURRENT_SOURCE_DIR}/gui/ui.json)
    endif()
    add_executable(pll_idle_tests tests/pll_idle_tests.cpp)
    target_link_libraries(pll_idle_tests PRIVATE emulator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pll_idle_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME pll_idle_tests COMMAND pll_idle_tests)
    add_executable(execution_accounting_tests tests/execution_accounting_tests.cpp)
    target_link_libraries(execution_accounting_tests PRIVATE emulator bfin-core)
    target_include_directories(execution_accounting_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Existing device interfaces have named unused virtual parameters.
        target_compile_options(execution_accounting_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME execution_accounting_tests COMMAND execution_accounting_tests)
    set_tests_properties(execution_accounting_tests PROPERTIES TIMEOUT 30)
    add_executable(guest_clock_tests tests/guest_clock_tests.cpp)
    target_link_libraries(guest_clock_tests PRIVATE emulator bfin-core)
    target_include_directories(guest_clock_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(guest_clock_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME guest_clock_tests COMMAND guest_clock_tests)
    set_tests_properties(guest_clock_tests PROPERTIES TIMEOUT 30)
    add_executable(guest_coretimer_tests tests/guest_coretimer_tests.cpp)
    target_link_libraries(guest_coretimer_tests PRIVATE emulator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(guest_coretimer_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME guest_coretimer_tests COMMAND guest_coretimer_tests)
    set_tests_properties(guest_coretimer_tests PROPERTIES TIMEOUT 30)
    add_executable(timing_profile_tests tests/timing_profile_tests.cpp)
    target_link_libraries(timing_profile_tests PRIVATE emulator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(timing_profile_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME timing_profile_tests COMMAND timing_profile_tests)
    set_tests_properties(timing_profile_tests PROPERTIES TIMEOUT 30)
    add_executable(nand_ecc_tests tests/nand_ecc_tests.cpp src/cpu/nand_ecc.cpp)
    target_include_directories(nand_ecc_tests PRIVATE src)
    add_test(NAME nand_ecc_tests COMMAND nand_ecc_tests)
    add_executable(functional_idle_tests tests/functional_idle_tests.cpp)
    target_link_libraries(functional_idle_tests PRIVATE emulator bfin-core)
    target_include_directories(functional_idle_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(functional_idle_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME functional_idle_tests COMMAND functional_idle_tests)
    set_tests_properties(functional_idle_tests PROPERTIES TIMEOUT 30)
    add_executable(sport_config_tests tests/sport_config_tests.cpp)
    target_link_libraries(sport_config_tests PRIVATE emulator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sport_config_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME sport_config_tests COMMAND sport_config_tests)
    add_executable(sport_guest_clock_tests tests/sport_guest_clock_tests.cpp)
    target_link_libraries(sport_guest_clock_tests PRIVATE emulator bfin-core)
    target_include_directories(sport_guest_clock_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sport_guest_clock_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME sport_guest_clock_tests COMMAND sport_guest_clock_tests)
    set_tests_properties(sport_guest_clock_tests PROPERTIES TIMEOUT 60)
    add_executable(usb_fastpath_tests tests/usb_fastpath_tests.cpp)
    target_link_libraries(usb_fastpath_tests PRIVATE emulator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # No -Wpedantic: usb.h's USBSetupBytes uses GNU anonymous structs.
        target_compile_options(usb_fastpath_tests PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME usb_fastpath_tests COMMAND usb_fastpath_tests)
    set_tests_properties(usb_fastpath_tests PROPERTIES TIMEOUT 120)
    add_executable(sic_level_interrupt_tests tests/sic_level_interrupt_tests.cpp)
    target_link_libraries(sic_level_interrupt_tests PRIVATE emulator bfin-core)
    target_include_directories(sic_level_interrupt_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sic_level_interrupt_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME sic_level_interrupt_tests COMMAND sic_level_interrupt_tests)
    set_tests_properties(sic_level_interrupt_tests PROPERTIES TIMEOUT 60)
    add_executable(dma_active_channel_tests tests/dma_active_channel_tests.cpp)
    target_link_libraries(dma_active_channel_tests PRIVATE emulator bfin-core)
    target_include_directories(dma_active_channel_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(dma_active_channel_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME dma_active_channel_tests COMMAND dma_active_channel_tests)
    set_tests_properties(dma_active_channel_tests PROPERTIES TIMEOUT 120)
    add_executable(multiblock_eligibility_tests tests/multiblock_eligibility_tests.cpp)
    target_link_libraries(multiblock_eligibility_tests PRIVATE emulator bfin-core)
    target_include_directories(multiblock_eligibility_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # No -Wpedantic: usb.h's USBSetupBytes uses GNU anonymous structs.
        target_compile_options(multiblock_eligibility_tests PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME multiblock_eligibility_tests COMMAND multiblock_eligibility_tests)
    set_tests_properties(multiblock_eligibility_tests PROPERTIES TIMEOUT 180)
    add_executable(multiblock_execution_tests tests/multiblock_execution_tests.cpp)
    target_link_libraries(multiblock_execution_tests PRIVATE emulator bfin-core)
    target_include_directories(multiblock_execution_tests PRIVATE ext/bcore/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # No -Wpedantic: usb.h's USBSetupBytes uses GNU anonymous structs.
        target_compile_options(multiblock_execution_tests PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME multiblock_execution_tests COMMAND multiblock_execution_tests)
    set_tests_properties(multiblock_execution_tests PROPERTIES TIMEOUT 600)
    add_executable(pcm_sample_tests tests/pcm_sample_tests.cpp)
    target_include_directories(pcm_sample_tests PRIVATE gui)
    add_test(NAME pcm_sample_tests COMMAND pcm_sample_tests)
    add_executable(audio_ring_tests tests/audio_ring_tests.cpp)
    target_include_directories(audio_ring_tests PRIVATE gui ${miniaudio_SOURCE_DIR})
    target_link_libraries(audio_ring_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME audio_ring_tests COMMAND audio_ring_tests)
    add_executable(canvas_mapping_tests tests/canvas_mapping_tests.cpp)
    target_include_directories(canvas_mapping_tests PRIVATE gui)
    add_test(NAME canvas_mapping_tests COMMAND canvas_mapping_tests)
    add_executable(otp_storage_tests tests/otp_storage_tests.cpp)
    target_link_libraries(otp_storage_tests PRIVATE emulator)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(otp_storage_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-parameter)
    endif()
    add_test(NAME otp_storage_tests COMMAND otp_storage_tests)
    set_tests_properties(otp_storage_tests PROPERTIES TIMEOUT 60)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ui_keymap_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_keymap_tests.py)
    endif()
endif()
