cmake_minimum_required(VERSION 3.10)
project(yoinkboard)

set(CMAKE_CXX_STANDARD 17)

# ── ImGui sources — identical for native and web ────────────────────────────
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

# ── App sources — identical for native and web ──────────────────────────────
# segmenter.{cpp,h} / platform.h declare the same interface on both targets;
# only their platform-specific Impl differs (segmenter_web.cpp/
# platform_web.cpp vs segmenter.cpp/platform_native.cpp below), so main.cpp
# and text_tool.cpp need zero #ifdefs for any of this.
set(COMMON_SOURCES
    main.cpp
    text_tool.cpp
    stb_image_impl.cpp
    ${IMGUI_SOURCES}
)

if(EMSCRIPTEN)
    # ── Web build (emcmake cmake .. && cmake --build .) ─────────────────────
    add_executable(yoinkboard
        ${COMMON_SOURCES}
        platform_web.cpp
        segmenter_web.cpp
    )
    set_target_properties(yoinkboard PROPERTIES SUFFIX ".html")

    target_include_directories(yoinkboard PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Note: For GLFW we use emscripten's contrib.glfw3 port (rather than the
    # older `-sUSE_GLFW=3`) for better High DPI support — verified working
    # against our exact pinned ImGui commit in web-port stage 1.
    set(EMS_SHARED_FLAGS
        -sDISABLE_EXCEPTION_CATCHING=1
        --use-port=contrib.glfw3
    )
    target_compile_options(yoinkboard PRIVATE ${EMS_SHARED_FLAGS})
    target_link_options(yoinkboard PRIVATE
        ${EMS_SHARED_FLAGS}
        -sWASM=1
        -sALLOW_MEMORY_GROWTH=1
        -sNO_EXIT_RUNTIME=0
        -sASSERTIONS=1
        # Bundle fonts/ into the virtual filesystem at /fonts so
        # TextTool::loadFonts()'s existing fopen("fonts/...") calls resolve
        # unchanged — no code differences needed between native and web here.
        --preload-file ${CMAKE_CURRENT_SOURCE_DIR}/fonts@fonts
        --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell_minimal.html
    )
else()
    # ── Native build (macOS) ─────────────────────────────────────────────────
    set(OpenGL_GL_PREFERENCE GLVND)
    add_definitions(-DGL_SILENCE_DEPRECATION) # OpenGL still works fine, just deprecated by Apple

    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)

    # ONNX Runtime
    set(ONNXRUNTIME_ROOT "/opt/homebrew/Cellar/onnxruntime/1.29.0")
    include_directories(${ONNXRUNTIME_ROOT}/include/onnxruntime)
    include_directories(${ONNXRUNTIME_ROOT}/include)
    link_directories(${ONNXRUNTIME_ROOT}/lib)

    add_executable(yoinkboard
        ${COMMON_SOURCES}
        segmenter.cpp
        platform_native.cpp
        tinyfiledialogs.c
    )

    target_include_directories(yoinkboard PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(yoinkboard PRIVATE
        glfw
        OpenGL::GL
        ${CMAKE_DL_LIBS}
        onnxruntime
    )

    set_source_files_properties(tinyfiledialogs.c PROPERTIES LANGUAGE C)
endif()
