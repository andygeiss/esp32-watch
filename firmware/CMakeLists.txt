# The ESP-IDF project. The CMakeLists.txt at the repo root builds the portable
# half against SDL and a window; this one builds the same files against the
# panel. Everything host-specific lives in main/, the way it lives in the root
# main.c on the other side.
cmake_minimum_required(VERSION 3.16)

# The pinned LVGL checkout at the repo root, added as a component rather than
# pulled from the ESP registry: the firmware and the simulator have to be the
# same LVGL down to the tag, and a registry dependency would float.
#
# It also settles lv_conf.h. LVGL's own esp.cmake registers ${LVGL_ROOT_DIR}/..
# as a PUBLIC include directory and defines LV_CONF_INCLUDE_SIMPLE, so putting
# lvgl/ here makes the repo root — where lv_conf.h sits — reachable from every
# component that requires lvgl, LVGL's own sources included. That is the same
# thing add_compile_definitions() and include_directories() do in the host
# build, and it has to be in place for the same reason.
get_filename_component(KAI_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${KAI_ROOT}/lvgl")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kai_watch)
