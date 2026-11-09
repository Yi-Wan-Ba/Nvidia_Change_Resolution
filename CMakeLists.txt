cmake_minimum_required(VERSION 3.20)
project(NvChange LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
    if(MSVC)
        set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    else()
        set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    endif()
endif()

add_executable(NvChange WIN32
    src/NvChangeGui.cpp
    resources.rc
)
target_include_directories(NvChange PRIVATE include)
target_compile_features(NvChange PRIVATE cxx_std_20)
target_link_libraries(NvChange PRIVATE gdiplus dwmapi ole32 uuid)

add_executable(NvChangeCli src/NvChange.cpp)
target_include_directories(NvChangeCli PRIVATE include)
target_compile_features(NvChangeCli PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(NvChange PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(NvChangeCli PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(NvChange PRIVATE -Wall -Wextra)
    target_compile_options(NvChangeCli PRIVATE -Wall -Wextra)
    target_link_options(NvChange PRIVATE -static)
    target_link_options(NvChangeCli PRIVATE -static)
endif()

install(TARGETS NvChange NvChangeCli RUNTIME DESTINATION .)
