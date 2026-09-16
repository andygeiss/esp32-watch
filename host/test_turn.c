/**
 * @file test_turn.c
 * The checks on voice/turn.c that need no server, no microphone and no LVGL:
 * the words. What wakes the watch, what puts it back to sleep, and what does
 * neither. The rest of that file is exercised by `make bench` against a real
 * server; this is the part that can be wrong on a Mac with nothing on it.
 *
 * The shape is test_ui.c's: a counter, a CHECK, and an exit status. Every
 * check here has been made to fail on purpose once, by the change it guards
 * against, and the comment beside it says which.
 */

#include "turn.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failed++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

static void goodbye_checks(void)
{
    /* The word on its own, however the transcriber dressed it. */
    CHECK(voice_is_goodbye("tschüss"), "the plain word");
    CHECK(voice_is_goodbye("Tschüss!"), "capital and an exclamation mark");
    CHECK(voice_is_goodbye(" stop. "), "stop, with space around it");
    CHECK(voice_is_goodbye("Quit"), "quit");

    /* The word and the name: the natural way to say goodbye to something that
     * has one. This is the first thing the hardware said that the code did
     * not understand — "Tschüss, Lizzie." was answered rather than obeyed,
     * and the watch stayed awake to answer the room for 30 s. Fails against
     * the whole-transcript rule on its own. */
    CHECK(voice_name_set("Lizzie"), "a name that fits");
    CHECK(voice_is_goodbye("Tschüss, Lizzie."), "the word and the name");
    CHECK(voice_is_goodbye("tschüss lizzie"), "the word and the name, lower case");
    CHECK(voice_is_goodbye("Stop, Lizzie!"), "another word and the name");
    CHECK(voice_name_set(""), "back to the default name");
    CHECK(voice_is_goodbye("Tschüss, Kai."), "the word and the default name");
    CHECK(!voice_is_goodbye("Tschüss, Lizzie."), "the old name no longer ends it");

    /* The small change people put around a goodbye, and the name first as
     * well as last. The first goodbye said to the hardware that was not
     * understood came dressed like this. */
    CHECK(voice_is_goodbye("Kai, tschüss"), "the name first");
    CHECK(voice_is_goodbye("Danke, tschüss!"), "thanks in front");
    CHECK(voice_is_goodbye("Ok, bis später, Kai."), "bis später and the name");
    CHECK(voice_is_goodbye("Auf Wiedersehen."), "two words of goodbye");
    CHECK(voice_is_goodbye("Gute Nacht, Kai"), "good night");

    /* The name as the transcriber spells it: any wake spelling's tail is a
     * name too, or a watch that wakes to "hey lissi" cannot be told
     * "tschüss, Lissi". */
    CHECK(voice_wake_set("hey lizzie|hey lissi|hey lizzy"), "three spellings");
    CHECK(voice_is_goodbye("Tschüss, Lissi."), "a wake spelling's name");
    CHECK(voice_is_goodbye("Tschüss Lizzy!"), "another");
    CHECK(voice_wake_set(""), "back to the built-in list");

    /* Anything more is something to answer: a goodbye that accepted any
     * other word would turn "stopp mal die Musik" back into an exit, which
     * is the substring rule this was written to avoid. */
    CHECK(!voice_is_goodbye("stopp mal die Musik"), "a sentence starting with a goodbye");
    CHECK(!voice_is_goodbye("tschüss sagen wir später"), "a goodbye with words after");
    CHECK(!voice_is_goodbye("tschüss Kaiser"), "the name as a prefix of another word");
    CHECK(!voice_is_goodbye("stopper"), "the word as a prefix of another word");
    CHECK(!voice_is_goodbye("Danke dir, alles gut"), "filler with no goodbye in it");
    CHECK(!voice_is_goodbye("ok danke bis dann tschüss und gute nacht"), "too many words to be a goodbye");
    CHECK(!voice_is_goodbye(""), "nothing at all");
}

static void wake_checks(void)
{
    const char * rest;

    CHECK(voice_wake_set(""), "the built-in list");
    rest = voice_after_wake("Hey Kai, hallo");
    CHECK(rest != NULL && strcmp(rest, "hallo") == 0, "what follows the name is the turn");
    rest = voice_after_wake("Hey, Kai.");
    CHECK(rest != NULL && *rest == '\0', "punctuation inside the greeting, nothing after");
    CHECK(voice_after_wake("Kaiser Wilhelm") == NULL, "the name inside another word");

    CHECK(voice_wake_set("hey ada|hi ada"), "a configured list");
    CHECK(voice_wake_count() == 2, "two spellings");
    rest = voice_after_wake("Hi Ada, wie spät ist es?");
    CHECK(rest != NULL && strcmp(rest, "wie spät ist es?") == 0, "the second spelling");
    CHECK(voice_after_wake("Hey Kai") == NULL, "the default no longer wakes it");
    CHECK(voice_wake_set(""), "back to the built-in list");
}

int main(void)
{
    goodbye_checks();
    wake_checks();
    printf("%d checks, %d failed\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
