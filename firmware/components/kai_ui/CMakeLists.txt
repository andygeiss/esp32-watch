# The portable half, compiled from ui/ unchanged: the same four files the
# simulator's kai_ui target builds, from the same paths. Nothing in host/ is
# reachable from here, and that is the boundary rule as a directory listing.
#
# REQUIRES lvgl and nothing else, which is the host/device boundary from
# CLAUDE.md written into the firmware build. An ESP-IDF header in ui.c would
# not be on this component's include path, so the build would stop — the same
# gate `make check` keeps with nm on the host side, enforced here by having
# nothing else to reach for.
get_filename_component(KAI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_register(
    SRCS "${KAI_ROOT}/ui/ui.c"
         "${KAI_ROOT}/ui/ui_font_digits_118.c"  # generated, see tools/gen_fonts.py
         "${KAI_ROOT}/ui/ui_font_date_72.c"
         "${KAI_ROOT}/ui/ui_font_assistant_18.c"
    INCLUDE_DIRS "${KAI_ROOT}/ui"
    REQUIRES lvgl)
