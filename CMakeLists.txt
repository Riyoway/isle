cmake_minimum_required(VERSION 3.28)
project(Isle VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Isle is a Windows-native application. Configure this project on Windows.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(Isle WIN32
    src/app/main.cpp

    src/core/ActivityStore.cpp
    src/core/Settings.cpp

    src/platform/Autostart.cpp
    src/platform/MonitorPlacement.cpp
    src/platform/OverlayWindow.cpp
    src/platform/Renderer.cpp
    src/platform/ShortcutEditor.cpp
    src/platform/TrayIcon.cpp

    src/providers/SystemProvider.cpp
    src/providers/AIUsageProvider.cpp
    src/providers/MediaProvider.cpp
    src/providers/ShortcutProvider.cpp
    src/plugins/PluginHost.cpp
)

target_include_directories(Isle PRIVATE src src/app)

target_compile_definitions(Isle PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(Isle PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)

    # Let LINK/mt.exe own manifest generation and merge our application
    # manifest into the linker-generated manifest. Embedding app.manifest as
    # RT_MANIFEST in resources.rc as well would create two resource ID 1
    # manifests and fail with CVT1100.
    target_link_options(Isle PRIVATE
        /MANIFEST:EMBED
        /MANIFESTUAC:NO
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/app.manifest"
    )
endif()

target_link_libraries(Isle PRIVATE
    d3d11
    dxgi
    d2d1
    dwrite
    dcomp
    dwmapi
    windowscodecs
    shell32
    shlwapi
    ole32
    runtimeobject
    windowsapp
    advapi32
    pdh
    winhttp
    comctl32
)

add_custom_command(TARGET Isle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Isle>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/InterVariable.ttf"
            "$<TARGET_FILE_DIR:Isle>/fonts/InterVariable.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL.txt"
            "$<TARGET_FILE_DIR:Isle>/fonts/OFL.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
            "$<TARGET_FILE_DIR:Isle>/icons"
)

set_target_properties(Isle PROPERTIES
    OUTPUT_NAME "Isle"
)

install(TARGETS Isle RUNTIME DESTINATION .)
install(FILES assets/fonts/InterVariable.ttf assets/fonts/OFL.txt DESTINATION fonts)
install(DIRECTORY assets/icons/ DESTINATION icons FILES_MATCHING PATTERN "*.svg")

include(CTest)
if(BUILD_TESTING)
    add_executable(IsleCoreTests
        tests/core_tests.cpp
        src/core/ActivityStore.cpp
    )
    target_include_directories(IsleCoreTests PRIVATE src)
    target_compile_features(IsleCoreTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(IsleCoreTests PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    endif()
    add_test(NAME IsleCoreTests COMMAND IsleCoreTests)
endif()
