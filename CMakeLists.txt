cmake_minimum_required(VERSION 3.16)
project(vk_flip_meter VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Find Vulkan ───────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# ── Layer shared library ──────────────────────────────────────────────────────
add_library(vk_flip_meter SHARED src/flip_meter.cpp)

target_include_directories(vk_flip_meter PRIVATE
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_options(vk_flip_meter PRIVATE
    -Wall -Wextra -O2 -fvisibility=hidden
)

target_link_libraries(vk_flip_meter PRIVATE
    ${CMAKE_THREAD_LIBS_INIT}
)

find_package(Threads REQUIRED)
target_link_libraries(vk_flip_meter PRIVATE Threads::Threads)

set_target_properties(vk_flip_meter PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "vk_flip_meter"
)

# ── Install targets ───────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS vk_flip_meter
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES manifest/VkLayer_cpu_flip_meter.json
    DESTINATION /usr/local/share/vulkan/implicit_layer.d/
)

# ── Build info ────────────────────────────────────────────────────────────────
message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
