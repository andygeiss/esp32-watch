# Glossary: ESP32 Watch

The words this project uses. One word per concept; where people say it more
than one way, the runners-up sit under _Avoid_. Wherever a concept shows up —
in the code, in a constant, in a commit — it shows up under the word listed
here.

**Assistant view** — the face with the two eyes in it. One of the two views;
the other is the clock view. Nothing on it can be pressed: the watch arrives
here on the wake phrase and leaves on the goodbye. _Avoid: eye mode, face mode,
assistant mode._

**Awake** — the state of having heard the wake phrase and not yet the goodbye.
The voice loop's, not the UI's: `voice_awake()` is what the assistant view is
up for, and `host/main.c` polls it into `ui_view_set()` ten times a second.
_Avoid: active, listening, session._

**Blink** — the infinite animation each eye runs while the assistant view is
up: 70 ms shut, 70 ms open, then 3.6 s held open. It drives the same property
the morph does, which is what makes switching back to the clock cancel it with
no bookkeeping, and the pupil closes with it because the lids leave nothing
between them. _Avoid: wink, idle animation._

**Catchlight** — the amber dot in an eye's pupil, up and to the left in both
eyes because there is one light in the room. Thirty per cent of the pupil and
offset by a fifth of it, so a blink takes it along. It is small and it is most
of what tells an eye from a dot. _Avoid: highlight, glint, sparkle, spark._

**Cell** — the fixed advance every digit occupies in a generated font, 79 px
in the 118 px face. `lv_font_conv` has no monospace switch, so `gen_fonts.py`
rewrites each digit to the widest digit's advance with its ink centred in that
cell. Punctuation keeps its own advance and gets no cell. _Avoid: slot, box,
advance width._

**Clock view** — the face with the time, the date and its weekday on it. The
stack sits in the middle of the whole panel; nothing along the bottom reserves
a strip of it. _Avoid: watch face, time mode, default view._

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
comes closer than 32 px to an edge of the panel. The 94 px group offset and the
four corner readouts are derived from it, and it is what caps the digits at
118 px. _Avoid: padding, inset, safe area._

**Eye** — one of the two amber circles the assistant looks out of, 69 px
across, with a pupil, a catchlight and four lashes. Each starts life as its
digit group's own box — same size, same centre, already round, invisible — so
the morph is that box changing shape rather than a new thing appearing. It is
narrower than that box on purpose: what is left black between the two is what
makes them read as a pair rather than as two discs. _Avoid: iris, dot, ball._

**Gate** — the loudness a block of microphone audio has to clear to count as
speech, and the 800 ms below it that ends a turn. Measured, not chosen: a
quiet room reads a mean RMS of 42, speech runs in the thousands, and
`VOICE_SILENCE_RMS` sits at 500 between them. The same measurement is what
lets it call a device dead: a room never reads zero, so three seconds of
exact zeros is a microphone that is not hearing rather than a room with
nobody in it. _Avoid: VAD, threshold, endpoint detection._

**Goodbye** — a word that ends the session and hands the watch back to the
clock: `tschüss`, `quit`, `stop`, matched against the whole transcript so that
a sentence merely containing one is answered rather than obeyed. Its partner is
the idle timeout, 30 s with nothing said, which needs no word at all. _Avoid:
stop word, sleep phrase, dismiss._

**Group** — the hour pair or the minute pair, as one object. There are two,
placed symmetrically about the centre, and they stay separate because they are
the two eyes: never merged into one label, never given a separator between
them. _Avoid: digit pair, half, cluster._

**Heard** — the transcript of one turn, as Parakeet returned it. It is what
goes to the brain, and with no brain configured it is also what comes back:
the point of the echo is to hear what the microphone and the transcriber
actually produced. _Avoid: utterance, query, prompt._

**Brain** — the chat model that answers, and the seam it sits behind.
`reply()` is that seam on both builds: `Qwen3.8-27B-oQ4e-mtp` by default, any
model `WATCH_BRAIN_MODEL` or `CONFIG_WATCH_BRAIN_MODEL` names, and the heard
text said back when either says **echo**. Not "the LLM" and not "the AI" — the
watch has three services and this is the one that thinks. Thinking is all it
does: it knows nothing about this wrist, and what it needs from there it asks
for with a [[Tool]]. _Avoid: LLM, model (unqualified), AI, assistant._

**Host** — the machine a build runs the simulator on, as opposed to the device.
Also the half of any build that knows which of the two it is: window or panel,
mouse or touch, `SDL_GetTicks` or `esp_timer`. _Avoid: PC, desktop, simulator
side._

**Lash** — one of the four amber strokes on an eye's upper-outer arc, and the
whole of what makes the pair a woman's rather than anybody's. Longest at the
outer corner, 22 px, shortening to 14 px going in; the right eye's four are the
left's mirrored about the vertical. Drawn by the eye rather than built out of
objects, because nothing that can be sized in percent stays put on an eye that
is not square. _Avoid: eyelash, spoke, ray, whisker._

**Lid** — the eye's own padding, `UI_EYE_LID`, 6 px on every side. Half the
12 px the eye shuts to, so the top and bottom meet exactly as the blink bottoms
out, and the pupil measured against what they leave is pinched out at the same
moment. _Avoid: eyelid, inset, margin._

**Morph** — the 400 ms change from one view to the other: the digits fade off
while the box behind them pulls in square and round into an eye, and back
again. It is the point of the project, and nothing here may foreclose it.
_Avoid: transition, animation, morphing._

**Panel** — the 410 x 502 AMOLED itself. The simulator window is the same size
at 1:1, which is the whole reason the simulator is worth having. _Avoid:
screen, display, LCD._

**Portable half** — the files both builds compile unchanged. There are two
directories of them: `ui/`, which may call LVGL and the C standard library,
and `voice/`, which may call the C standard library and nothing else. Their
opposite numbers are `host/` and `firmware/main/`, and there is no `src/`
holding any of it. _Avoid: shared code, common, core._

**Pupil** — the hole in an eye: a disc the colour of the background, half of
what the lids leave between them. A percentage rather than a size of its own,
which is what makes the morph and the blink carry it — nothing animates it
directly, and nothing should. _Avoid: iris, hole, centre._

**Reference clip** — `voices/female.wav` and the transcript beside it, the
recording the synthesiser borrows a voice from.
`chatterbox-multilingual-v3` ships no voices of its own and answers 500
without one, so it is a prerequisite rather than a setting. Gitignored: it is
a recording of a person. The simulator reads it at start-up; the firmware
links it into flash, and only if it is there. _Avoid: sample, voice file,
speaker prompt._

**Status** — the struct `ui_status_t` and its setter, how a fact the UI cannot
reach for itself gets in: the charge, the radio, the microphone, the speaker.
The simulator fills it with a fake, the firmware fills it from the hardware,
and `ui.c` never learns which. `ui_view_set()` is the same crossing for which
face is up, and nothing crosses the other way. _Avoid: state, model, context._

**Strip** — one flush's worth of pixels: a horizontal slice of a dirty area, as
many rows tall as the 41 kB draw buffer holds, rendered and then sent over DMA.
The panel takes its coordinates in pairs, so a strip always starts on an even
row and ends on an odd one. _Avoid: chunk, tile, block, band._

**Tabular figures** — every digit sharing one advance, so a centred label does
not shift sideways when the time changes. Montserrat's figures are proportional
— at 118 px a `1` is 44 px where a `0` is 79 — and at this size the shift is
impossible to miss. `--no-kerning` belongs to the same decision. _Avoid:
monospace digits, fixed-width numerals._

**Tool** — something the brain can ask the watch for because the watch knows
it and a server cannot. Two so far, both the clock: `get_time` and `get_date`,
reading the same `time()` the face is drawn from, so what is said and what is
shown cannot disagree. A tool is a name, a sentence telling the model when to
reach for it, and a function returning a short string — all portable, so both
builds have the same ones. Not a command and not a skill: the wearer never
names one, the brain does. _Avoid: function, function call, capability, skill,
action._

**Turn** — one exchange: a sentence arrives, is transcribed, answered and
played back. The voice loop takes turns for as long as the assistant is awake.
Nothing interrupts one in progress — talking over the assistant needs the echo
cancellation this half-duplex loop has none of. _Avoid: exchange, round,
interaction._

**View** — the clock view or the assistant view. There is one screen and two
views on it, and the morph is how it gets from one to the other. _Avoid:
screen, page, mode._

**Voice loop** — the microphone, Parakeet, the brain, Chatterbox and the
speaker, on a thread of its own, and the reason the bottom two corners are not
faked. Three files: `voice/turn.c` is the portable half both builds share,
`host/voice.c` drives SDL and a hand-written request over a socket or OpenSSL,
`firmware/main/voice.c` drives the board's two codecs and `esp_http_client`.
The two platform halves are the same shape on purpose. _Avoid: audio pipeline,
speech stack, assistant backend._

**Name** — what the assistant calls itself, `Kai` by default and
`WATCH_NAME` or `CONFIG_WATCH_NAME` otherwise. It lives in the system prompt
and is the answer to "who are you?". Not the same thing as the wake phrase,
which is what the microphone listens for: the one is spoken, the other is
heard, and the spellings of the second are the transcriber's rather than
anyone's choice. _Avoid: persona, identity, assistant name (unqualified)._

**Wake phrase** — `Hey Kai`, and the thing that puts the assistant view up.
There is no wake-word engine on either build, so the microphone stays open,
every utterance is transcribed, and the phrase is looked for in the text —
`WAKE` in `voice/turn.c` is a table because the transcriber has never been
shown the name and spells it several ways. Whatever follows it is the first
turn. The phrase is configuration and the table is its default:
`WATCH_WAKE_PHRASE` on the host and `CONFIG_WATCH_WAKE_PHRASE` on the device
hand `voice_wake_set()` a `|`-separated list, and an empty one means the
table. On the watch the transcriber-as-wake-word is a choice rather than a
necessity: ESP-SR would hear it locally, but none of WakeNet's models is this
name. Renaming the assistant is [[Name]], a separate setting. _Avoid: wake
word, hotword, trigger._
