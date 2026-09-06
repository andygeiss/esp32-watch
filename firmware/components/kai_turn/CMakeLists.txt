# The portable half of the voice loop, compiled from voice/ unchanged: the
# same file the simulator's kai_turn target builds, from the same path. The
# words the watch answers to, the gate that ends a turn and the two request
# bodies are all in there, so the watch and the simulator cannot drift apart
# about any of them.
#
# REQUIRES nothing at all — not even lvgl. That is one notch stricter than
# kai_ui beside it, and it is the whole claim turn.c makes about itself: the C
# standard library and no more. An ESP-IDF header in it would not be on this
# component's include path and the build would stop, which is the same gate
# `make check` keeps with nm on the host side.
get_filename_component(KAI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_register(
    SRCS "${KAI_ROOT}/voice/turn.c"
    INCLUDE_DIRS "${KAI_ROOT}/voice")
