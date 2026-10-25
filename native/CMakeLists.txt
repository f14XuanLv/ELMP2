# elmp_base: platform layer without UE4SS (CTest-able). ELMP / ELMP_Dev: the mod (dlls/main.dll).
# ElmpFrameProbe: Dev-only measurement mod (frame intervals with or without ELMP loaded).

add_library(elmp_base STATIC
    base/log.cpp
    base/clock.cpp
    base/json.cpp
    base/binary_manifest.cpp
    base/preflight.cpp
    base/metrics.cpp
    base/events.cpp
    base/slots.cpp
    base/settings.cpp
)
target_include_directories(elmp_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ELMP_GENERATED_DIR})
target_compile_definitions(elmp_base PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(elmp_base PRIVATE /W4 /utf-8)
target_link_libraries(elmp_base PUBLIC bcrypt)
set_target_properties(elmp_base PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(ELMP_MOD_SOURCES
    base/hook.cpp
    engine/ue.cpp
    engine/reflect.cpp
    engine/registry.cpp
    engine/frame.cpp
    engine/lifecycle.cpp
    engine/world.cpp
    engine/binary_checks.cpp
    core/schema.cpp
    core/player.cpp
    core/spawn.cpp
    core/profile.cpp
    core/vitals.cpp
    core/skin.cpp
    input/pad_state.cpp
    input/virtual_pad.cpp
    input/bindings.cpp
    input/router.cpp
    input/xinput.cpp
    input/rawinput.cpp
    party/config.cpp
    party/devices.cpp
    party/progress.cpp
    party/seat.cpp
    party/respawn.cpp
    party/bench.cpp
    party/rules.cpp
    party/transit.cpp
    party/camera_sync.cpp
    core/permission.cpp
    core/hitrules.cpp
    camera/framing.cpp
    pvp/hitgate.cpp
    console/console.cpp
    console/commands.cpp
    console/core_commands.cpp
    console/party_commands.cpp
    dev/lua_bridge.cpp
    mod/dllmain.cpp
)

function(elmp_add_mod target package dev)
    add_library(${target} SHARED ${ELMP_MOD_SOURCES})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${target} PRIVATE /utf-8)
    target_link_libraries(${target} PRIVATE elmp_base setupapi hid)
    target_link_libraries(${target} PUBLIC UE4SS)
    if(dev)
        target_compile_definitions(${target} PRIVATE ELMP_DEV=1)
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "main"
        ARCHIVE_OUTPUT_NAME "${target}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/${package}/dlls"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/${package}/lib"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/${package}/dlls"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endfunction()

elmp_add_mod(ELMP_Dev dev ON)
elmp_add_mod(ELMP shipping OFF)

# Dev-only measurement mod: records EngineTick-to-EngineTick intervals so a bare-UE4SS baseline can be
# compared with ELMP loaded (G0 gate). Never packaged into Shipping.
add_library(ElmpFrameProbe SHARED dev/frameprobe/frameprobe.cpp)
target_include_directories(ElmpFrameProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ElmpFrameProbe PRIVATE /utf-8)
target_link_libraries(ElmpFrameProbe PRIVATE elmp_base)
target_link_libraries(ElmpFrameProbe PUBLIC UE4SS)
set_target_properties(ElmpFrameProbe PROPERTIES
    OUTPUT_NAME "main"
    ARCHIVE_OUTPUT_NAME "ElmpFrameProbe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/frameprobe/dlls"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/frameprobe/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/frameprobe/dlls"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

if(BUILD_TESTING)
    foreach(t test_manifest test_storage test_bus_slots)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE elmp_base)
        target_compile_options(${t} PRIVATE /utf-8)
        target_compile_definitions(${t} PRIVATE ELMP_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
        set_target_properties(${t} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()
