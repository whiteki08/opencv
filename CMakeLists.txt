cmake_minimum_required(VERSION 3.28)
project(image_resizer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)


# 添加 FFmpeg 头文件查找
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
find_path(SWSCALE_INCLUDE_DIR libswscale/swscale.h)

find_package(Qt5 COMPONENTS Widgets Charts REQUIRED)
find_package(OpenCV REQUIRED)

set(SOURCES
    main.cpp
    ImageResizer.cpp
    ImageResizer.h
)

add_executable(${PROJECT_NAME} ${SOURCES})

# 添加头文件包含路径
target_include_directories(${PROJECT_NAME} PRIVATE
    ${AVCODEC_INCLUDE_DIR}
    ${AVFORMAT_INCLUDE_DIR}
    ${SWSCALE_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME}
    Qt5::Widgets
    Qt5::Charts
    ${OpenCV_LIBS}
    avcodec
    avformat
    avutil
    swscale
)

# 添加平台检测
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_definitions(-DX86_64_PLATFORM)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    add_definitions(-DARM64_PLATFORM)
endif()

# 检测SIMD支持
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
endif()

# 添加编译选项以支持 SIMD
if(CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()

# 添加安装规则
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES ${QT_PLUGINS} DESTINATION plugins)