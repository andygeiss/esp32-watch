#!/bin/sh
# Makes voices/female.wav and voices/female.txt — the clip the assistant
# borrows its voice from, and the words it says.
#
# The voice is macOS's own Anna, which is synthetic and already on the
# machine, and the words are written here. That is the point of generating it
# rather than finding one: a reference clip is a voice to be cloned, so a
# recording of a person who did not agree to that is not a candidate, and a
# professional's demo reel least of all.
#
# Chatterbox aligns the clip against its transcript and rejects the audio on
# its own, so the two are written together and must stay in step. Change the
# text here and the clip is regenerated with it.
#
# voices/ is gitignored, so this is what a fresh clone runs to get a voice.
set -eu

VOICE=${VOICE:-Anna}
# About five seconds of it. The clip travels base64 in the body of every
# reply, so its length is paid for on every single turn — at 16 kHz mono that
# is 160 kB of WAV and 213 kB on the wire for five seconds, and twice that for
# ten. Long enough to condition a speaker, short enough not to be felt.
TEXT=${TEXT:-"Guten Morgen. Ich bin die Stimme deiner Uhr. Frag mich einfach, ich höre zu."}
OUT=${OUT:-voices/female}

command -v ffmpeg >/dev/null || { echo "needs ffmpeg: brew install ffmpeg" >&2; exit 1; }
# Against the list rather than by trying it: `say -v NoSuchVoice` exits 0 and
# quietly speaks in the default voice, so a typo would produce a clip in the
# wrong voice and say nothing about it. The name is everything before the
# locale column.
say -v '?' | sed -e 's/ *([^)]*).*//' -e 's/  *[a-z][a-z]_[A-Z][A-Z].*//' \
    | grep -qx "$VOICE" || {
    echo "no such system voice: $VOICE" >&2
    echo "German ones on this machine:" >&2
    say -v '?' | grep 'de_DE' | sed -e 's/ *([^)]*).*//' | sort -u | sed 's/^/  /' >&2
    exit 1
}

mkdir -p "$(dirname "$OUT")"
printf '%s\n' "$TEXT" > "$OUT.txt"

# 24 kHz out of `say`, then 16 kHz mono: the clip travels base64 in the body
# of every reply, so half the rate is half the request. Speaker conditioning
# does not need more than that.
say -v "$VOICE" --file-format=WAVE --data-format=LEI16@24000 -o "$OUT.raw.wav" "$TEXT"
ffmpeg -loglevel error -y -i "$OUT.raw.wav" -ac 1 -ar 16000 -c:a pcm_s16le "$OUT.wav"
rm -f "$OUT.raw.wav"

BYTES=$(wc -c < "$OUT.wav" | tr -d ' ')
SECONDS_=$(echo "$BYTES" | awk '{printf "%.1f", ($1 - 44) / 32000}')
echo "$OUT.wav  $BYTES bytes, ${SECONDS_}s, spoken by $VOICE"
echo "$OUT.txt  $TEXT"

# A warning rather than a failure: the clip still works, it just costs more on
# every reply than it needs to.
awk -v s="$SECONDS_" 'BEGIN { if (s > 6) exit 1 }' || \
    echo "warning: ${SECONDS_}s is longer than the ~5s this wants — shorten TEXT" >&2
