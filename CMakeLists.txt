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

# 7. 构建后自动拷贝FFmpeg、SDL2、whisper和llama的DLL到exe目录
file(GLOB FFMPEG_DLLS ${FFMPEG_ROOT}/bin/*.dll)
file(GLOB SDL2_DLLS ${SDL2_ROOT}/lib/*.dll)
file(GLOB WHISPER_DLLS ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/*.dll)
file(GLOB LLAMA_DLLS ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/llama*.dll ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/ggml*.dll)

add_custom_command(TARGET MediaPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${FFMPEG_DLLS}
    ${SDL2_DLLS}
    ${WHISPER_DLLS}
    ${LLAMA_DLLS}
    $<TARGET_FILE_DIR:MediaPlayer>
)
