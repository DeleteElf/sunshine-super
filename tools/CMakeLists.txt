cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

include_directories("${CMAKE_SOURCE_DIR}")

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES})
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES})
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

set(PROJECT_SVC_NAME "sunshinesvc")
message("add ${PROJECT_SVC_NAME}==============>")

add_executable(${PROJECT_SVC_NAME} sunshinesvc.cpp)
set_target_properties(${PROJECT_SVC_NAME} PROPERTIES CXX_STANDARD 23)
target_link_libraries(${PROJECT_SVC_NAME}
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(${PROJECT_SVC_NAME} PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
