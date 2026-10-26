cmake_minimum_required(VERSION 3.20)
project(MediaPlayer)

# 1. 配置Qt6路径（也可以通过命令行传入）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 2. 配置FFmpeg路径
set(FFMPEG_ROOT ${CMAKE_SOURCE_DIR}/third_party/ffmpeg)
include_directories(${FFMPEG_ROOT}/include)
link_directories(${FFMPEG_ROOT}/lib)

# 3. 配置SDL2路径
set(SDL2_ROOT ${CMAKE_SOURCE_DIR}/third_party/SDL2)
include_directories(${SDL2_ROOT}/include)
link_directories(${SDL2_ROOT}/lib)

# 3.5 集成 whisper.cpp（离线语音识别，T4.8）
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp)

# 3.6 集成 llama.cpp（离线翻译模型，T4.8）
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp)

# 4. 查找Qt6模块
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# 5. 添加可执行文件（qt_add_executable自动处理Windows入口点）
qt_add_executable(MediaPlayer
    main.cpp
    AppPaths.cpp
    AudioRingBuffer.cpp
    VadSegmenter.cpp
    SileroVad.cpp
    PlayerController.cpp
    VideoWidget.cpp
    D3D11Renderer.cpp
    SubtitleParser.cpp
    WhisperTranscriber.cpp
    Translator.cpp
    Logger.cpp
    CrashHandler.cpp
    SystemInfo.cpp
    DiagnosticPanel.cpp
    DiagnosticExporter.cpp
)

# 6. 链接所有库
target_link_libraries(MediaPlayer PRIVATE
    avcodec avformat avutil swscale swresample
    SDL2
    whisper
    llama
    Qt6::Core Qt6::Gui Qt6::Widgets
    d3d11 dxgi d3dcompiler
    dbghelp psapi
)

# 禁用SDL的main重定义，避免链接错误
target_compile_definitions(MediaPlayer PRIVATE SDL_MAIN_HANDLED)

# MSVC编译器增加堆空间（whisper.cpp头文件较大）
if(MSVC)
    target_compile_options(MediaPlayer PRIVATE /Zm2000)
endif()

# Release模式：去掉控制台窗口（Windows子系统）+ 禁用qDebug输出
set_target_properties(MediaPlayer PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
target_compile_definitions(MediaPlayer PRIVATE
    $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
    $<$<CONFIG:Release>:QT_NO_INFO_OUTPUT>
)

# 8. 无 GUI 的翻译回归工具：改 prompt/采样参数后用同一批语料对比前后输出
add_executable(translate_cli
    tools/translate_cli.cpp
    Translator.cpp
)
target_include_directories(translate_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(translate_cli PRIVATE llama Qt6::Core)

# 8.5 纯函数单元测试见文件末尾（依赖 LLAMA_DLLS 变量）

# 7. 构建后自动拷贝FFmpeg、SDL2、whisper和llama的DLL到exe目录
file(GLOB FFMPEG_DLLS ${FFMPEG_ROOT}/bin/*.dll)
file(GLOB SDL2_DLLS ${SDL2_ROOT}/lib/*.dll)

# llama/whisper 的 DLL 在配置期还不在 build 目录，必须用 target 生成表达式定位
set(LLAMA_DLLS $<TARGET_FILE:llama> $<TARGET_FILE:ggml> $<TARGET_FILE:ggml-base> $<TARGET_FILE:ggml-cpu>)
set(WHISPER_DLLS $<TARGET_FILE:whisper>)

add_custom_command(TARGET MediaPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${FFMPEG_DLLS}
    ${SDL2_DLLS}
    ${WHISPER_DLLS}
    ${LLAMA_DLLS}
    $<TARGET_FILE_DIR:MediaPlayer>
)

add_custom_command(TARGET translate_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${LLAMA_DLLS}
    $<TARGET_FILE_DIR:translate_cli>
)

# 8.6 无 GUI 的 VAD 分段校验：音频 -> Silero VAD -> 语音段（不需要视频文件）
add_executable(vad_cli
    tools/vad_cli.cpp
    SileroVad.cpp
    VadSegmenter.cpp
)
target_include_directories(vad_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vad_cli PRIVATE whisper Qt6::Core)

add_custom_command(TARGET vad_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${WHISPER_DLLS}
    $<TARGET_FILE_DIR:vad_cli>
)

# 9. 纯函数单元测试：不加载模型、不开窗口、不碰 D3D11，秒级返回
enable_testing()
add_executable(unit_tests
    tests/unit_tests.cpp
    AudioRingBuffer.cpp
    VadSegmenter.cpp
    Translator.cpp
    AppPaths.cpp
    Logger.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(unit_tests PRIVATE llama Qt6::Core)
add_custom_command(TARGET unit_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${LLAMA_DLLS}
    $<TARGET_FILE_DIR:unit_tests>
)

# 10. 软解上传链路探针：不需要窗口和视频文件，直接验证 NV12 上传->拷贝->采样->回读
add_executable(d3d11_probe tools/d3d11_probe.cpp)
target_link_libraries(d3d11_probe PRIVATE d3d11 dxgi d3dcompiler)
add_test(NAME d3d11_probe COMMAND d3d11_probe)

add_test(NAME unit_tests COMMAND unit_tests)
# 测试进程要能加载 Qt6Core.dll：从 Qt 安装目录取 bin 路径注入 PATH
get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION_DEBUG)
if(NOT _qt_core_loc)
    get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
endif()
get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
set_tests_properties(unit_tests PROPERTIES ENVIRONMENT "PATH=${_qt_bin_dir};$ENV{PATH}")
