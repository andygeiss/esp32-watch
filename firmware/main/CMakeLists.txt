# The host layer, device edition. esp_lcd_sh8601, esp_lcd_touch_ft5x06 and
# esp_codec_dev are managed dependencies — see idf_component.yml — and the
# component manager adds them to the requirements itself.
#
# kai_ui and kai_turn are the two portable halves, compiled out of ui/ and
# voice/ from where they sit. This component is the only one that may name
# both, because it is the only one that is allowed to know it is a watch.
idf_component_register(
    SRCS "main.c" "board.c" "net.c" "voice.c"
    INCLUDE_DIRS "."
    REQUIRES kai_ui kai_turn lvgl driver esp_lcd esp_timer esp_wifi esp_netif
             esp_http_client esp-tls nvs_flash)

# The clip the assistant borrows its voice from, and the words it says. Both
# are gitignored, so a fresh clone has neither and the firmware has to build
# anyway. Without them CONFIG_WATCH_HAS_VOICE is 0, voice.c logs it, and the
# watch is a clock: the same answer the simulator gives when they are missing.
#
# The name is fixed here where the host takes a path, and that is not an
# oversight. target_add_binary_data derives the linker symbol from the file
# name, so voice.c can only name a symbol it already knows — on the device
# "which clip" is a build-time fact, and it always was. tools/gen_voice.sh
# makes the default one.
get_filename_component(KAI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(EXISTS "${KAI_ROOT}/voices/female.wav" AND EXISTS "${KAI_ROOT}/voices/female.txt")
    target_add_binary_data(${COMPONENT_LIB} "${KAI_ROOT}/voices/female.wav" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "${KAI_ROOT}/voices/female.txt" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_WATCH_HAS_VOICE=1)
    message(STATUS "WATCH: the reference clip is compiled in")
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_WATCH_HAS_VOICE=0)
    message(STATUS "WATCH: no voices/female.wav — the assistant will stay asleep")
endif()

# The network the watch joins, out of .env by way of `make firmware`: the
# Makefile sources it and passes WATCH_WLAN_SSID and WATCH_WLAN_PASS in as
# CMake variables, and here they become two string macros net.c reads. They
# are the host's settings list crossing over to the device the way the
# server address does, and they are not in Kconfig for the same reason the
# key is not compiled in on the host — the password is a secret, .env is
# where this repository keeps those, and a setting that lives in two places
# is set in one and forgotten in the other.
#
# Both macros always exist, empty when nothing was passed, so net.c can test
# the SSID for '\0' rather than for #ifdef. The two replaces are what make a
# value with a quote or a backslash in it a valid C string literal.
foreach(setting WATCH_WLAN_SSID WATCH_WLAN_PASS)
    set(value "${${setting}}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "${setting}=\"${value}\"")
endforeach()
if(WATCH_WLAN_SSID)
    message(STATUS "WATCH: the watch will join ${WATCH_WLAN_SSID}")
else()
    message(STATUS "WATCH: no WATCH_WLAN_SSID — the radio stays off")
endif()
