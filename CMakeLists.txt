cmake_minimum_required(VERSION 3.28)
project(dawproject_player LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(signalsmith-linear INTERFACE)
target_include_directories(signalsmith-linear INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include)

add_library(signalsmith-stretch INTERFACE)
target_include_directories(signalsmith-stretch INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include)
target_link_libraries(signalsmith-stretch INTERFACE signalsmith-linear)

add_library(miniz STATIC
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c)
target_include_directories(miniz PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
target_compile_options(miniz PRIVATE -w)

add_library(dawplay STATIC
    src/timeline.cpp
    src/xml.cpp
    src/project.cpp
    src/sources.cpp
    src/engine.cpp
    src/miniaudio_impl.cpp
    src/wasm_api.cpp)
target_include_directories(dawplay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
target_link_libraries(dawplay PUBLIC miniz signalsmith-stretch)
target_compile_options(dawplay PRIVATE -Wall -Wextra)
set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

find_package(Threads REQUIRED)
target_link_libraries(dawplay PUBLIC Threads::Threads m)
if(CMAKE_DL_LIBS)
    target_link_libraries(dawplay PUBLIC ${CMAKE_DL_LIBS})
endif()

if(NOT EMSCRIPTEN)
    add_executable(dawplay_cli src/main.cpp)
    target_link_libraries(dawplay_cli PRIVATE dawplay)
    set_target_properties(dawplay_cli PROPERTIES OUTPUT_NAME dawplay)

    add_executable(dawplay_tests tests/test_player.cpp)
    target_link_libraries(dawplay_tests PRIVATE dawplay)
    target_compile_options(dawplay_tests PRIVATE -Wall -Wextra)
endif()

if(EMSCRIPTEN)
    add_executable(dawplay_web src/wasm_main.cpp)
    target_link_libraries(dawplay_web PRIVATE dawplay)
    target_compile_options(dawplay_web PRIVATE -pthread)
    target_link_options(dawplay_web PRIVATE
        -pthread
        -sPTHREAD_POOL_SIZE=4
        -sAUDIO_WORKLET=1
        -sWASM_WORKERS=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,HEAPU8
        -sEXPORTED_FUNCTIONS=_main,_daw_create,_daw_destroy,_daw_resize_input,_daw_load,_daw_play,_daw_pause,_daw_stop,_daw_seek_seconds,_daw_seek_bar,_daw_set_preset,_daw_set_hardware,_daw_set_cache_mib,_daw_start_device,_daw_render_start,_daw_render_state,_daw_render_data,_daw_render_size,_daw_status_json,_malloc,_free)
    set_target_properties(dawplay_web PROPERTIES OUTPUT_NAME dawplay SUFFIX ".js")
    add_custom_command(TARGET dawplay_web POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/serve.py
            $<TARGET_FILE_DIR:dawplay_web>)
endif()
