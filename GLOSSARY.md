# Glossary: KAI watch

The words this project uses. One word per concept; where people say it more
than one way, the runners-up sit under _Avoid_. Wherever a concept shows up —
in the code, in a constant, in a commit — it shows up under the word listed
here.

**Assistant view** — the face with the two eyes in it, and the `Quit` button.
One of the two views; the other is the clock view. _Avoid: eye mode, face mode,
assistant mode._

**Blink** — the infinite animation each eye runs while the assistant view is
up: 70 ms shut, 70 ms open, then 3.6 s held open. It drives the same property
the morph does, which is what makes switching back to the clock cancel it with
no bookkeeping. _Avoid: wink, idle animation._

**Cell** — the fixed advance every digit occupies in a generated font, 79 px
in the 118 px face. `lv_font_conv` has no monospace switch, so `gen_fonts.py`
rewrites each digit to the widest digit's advance with its ink centred in that
cell. Punctuation keeps its own advance and gets no cell. _Avoid: slot, box,
advance width._

**Clock view** — the face with the time, the date and its weekday on it, and
the `Hey Kai` button. _Avoid: watch face, time mode, default view._

**Corner** — one of the four status readouts, one per corner of the panel: the
radio and the charge along the top, the assistant's speaker and microphone
along the bottom. All four stay up in both views — they are a status layer over
whichever face is showing. _Avoid: badge, indicator, icon._

**Device** — the board a build targets, as opposed to the host. Half of the
boundary rule: `ui/` runs on both, `host/` runs on the host, `firmware/main/`
runs on the device — the directory is the rule. _Avoid: target, hardware, embedded side._

**Dim** — how a corner draws a reading the platform does not have: `LV_OPA_30`,
never hidden. An empty corner reads as a bug, a dim one reads as "no". It is
`text_opa` rather than the object's own opacity, so it survives the view fades.
_Avoid: grey out, disable, hide._

**Edge margin** — the layout rule, and the constant `UI_EDGE_MARGIN`: nothing
comes closer than 32 px to an edge of the panel. The 94 px group offset, the
vertical centring of the time-and-date stack and the button's distance from the
bottom are all derived from it, and it is what caps the digits at 118 px.
_Avoid: padding, inset, safe area._

**Eye** — one of the two amber circles the assistant looks out of. Each starts
life as its digit group's own box — same size, same centre, already round,
invisible — so the morph is that box changing shape rather than a new thing
appearing. _Avoid: pupil, iris, dot._

**Gate** — the loudness a block of microphone audio has to clear to count as
speech, and the 800 ms below it that ends a turn. Measured, not chosen: a
quiet room reads a mean RMS of 42, speech runs in the thousands, and
`VOICE_SILENCE_RMS` sits at 500 between them. _Avoid: VAD, threshold, endpoint
detection._

**Group** — the hour pair or the minute pair, as one object. There are two,
placed symmetrically about the centre, and they stay separate because they are
the two eyes: never merged into one label, never given a separator between
them. _Avoid: digit pair, half, cluster._

**Heard** — the transcript of one turn, as Parakeet returned it. It is what
the assistant says back, word for word, because the point of the echo is to
hear what the microphone and the transcriber actually produced. _Avoid:
utterance, query, prompt._

**Host** — the machine a build runs the simulator on, as opposed to the device.
Also the half of any build that knows which of the two it is: window or panel,
mouse or touch, `SDL_GetTicks` or `esp_timer`. _Avoid: PC, desktop, simulator
side._

**Morph** — the 400 ms change from one view to the other: the digits fade off
while the box behind them grows square and round into an eye, and back again.
It is the point of the project, and nothing here may foreclose it. _Avoid:
transition, animation, morphing._

**Panel** — the 410 x 502 AMOLED itself. The simulator window is the same size
at 1:1, which is the whole reason the simulator is worth having. _Avoid:
screen, display, LCD._

**Portable half** — `ui/`: `ui.c`, `ui.h` and the three generated fonts, the
files both builds compile unchanged. LVGL and the C standard library only. Its
opposite number is `host/`, and there is no `src/` holding both. _Avoid:
shared code, common, core._

**Reference clip** — `voices/kai.opus` and the transcript beside it, the
recording the synthesiser borrows a voice from.
`chatterbox-multilingual-v3` ships no voices of its own and answers 500
without one, so it is a prerequisite rather than a setting. Gitignored: it is
a recording of a person. _Avoid: sample, voice file, speaker prompt._

**Status** — the struct `ui_status_t` and its setter, the one way a fact the UI
cannot reach for itself gets in: the charge, the radio, the microphone, the
speaker. The simulator fills it with a fake, the firmware fills it from the
hardware, and `ui.c` never learns which. _Avoid: state, model, context._

**Strip** — one flush's worth of pixels: a horizontal slice of a dirty area, as
many rows tall as the 41 kB draw buffer holds, rendered and then sent over DMA.
The panel takes its coordinates in pairs, so a strip always starts on an even
row and ends on an odd one. _Avoid: chunk, tile, block, band._

**Tabular figures** — every digit sharing one advance, so a centred label does
not shift sideways when the time changes. Montserrat's figures are proportional
— at 118 px a `1` is 44 px where a `0` is 79 — and at this size the shift is
impossible to miss. `--no-kerning` belongs to the same decision. _Avoid:
monospace digits, fixed-width numerals._

**Turn** — one exchange: the microphone opens, a sentence arrives, it is
transcribed, answered and played back. The voice loop takes turns for as long
as the assistant is awake, and `Quit` ends the one in progress. _Avoid:
exchange, round, interaction._

**View** — the clock view or the assistant view. There is one screen and two
views on it, and the morph is how it gets from one to the other. _Avoid:
screen, page, mode._

**Voice loop** — `host/voice.c`: the microphone, Parakeet, Chatterbox and the
speaker, on a thread of its own. Host-only, and the reason the bottom two
corners are no longer faked. _Avoid: audio pipeline, speech stack, assistant
backend._
