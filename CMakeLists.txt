cmake_minimum_required(VERSION 3.10)
project(deskapp)

set(CMAKE_CXX_STANDARD 17)
set(OpenGL_GL_PREFERENCE GLVND)

# Silence macOS OpenGL deprecation warnings (OpenGL still works fine)
add_definitions(-DGL_SILENCE_DEPRECATION)

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

# ONNX Runtime
set(ONNXRUNTIME_ROOT "/opt/homebrew/Cellar/onnxruntime/1.29.0")
include_directories(${ONNXRUNTIME_ROOT}/include/onnxruntime)
include_directories(${ONNXRUNTIME_ROOT}/include)
link_directories(${ONNXRUNTIME_ROOT}/lib)

# ImGui sources
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_executable(deskapp
    main.cpp
    segmenter.cpp
    text_tool.cpp
    tinyfiledialogs.c
    stb_image_impl.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(deskapp PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(deskapp PRIVATE
    glfw
    OpenGL::GL
    ${CMAKE_DL_LIBS}
    onnxruntime
)

set_source_files_properties(tinyfiledialogs.c PROPERTIES LANGUAGE C)