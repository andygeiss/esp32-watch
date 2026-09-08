#!/usr/bin/env python3
"""Regenerate docs/watch.gif, the animation at the top of README.md.

    tools/gen_demo.py

Two halves. host/demo.c is the renderer: it drives the real ui.c off a fake
tick source and writes the frames it took, which is why the picture in the
README cannot drift from the code — it is the code, rendered at the panel's
own 410 x 502. This half turns those frames into a GIF.

Needs Pillow (pip install pillow), the way tools/gen_fonts.py needs node. It
builds first, so a stale kai_demo cannot be what gets rendered.

Every still stretch of the animation is already one frame with a long
duration, folded together by the renderer. This caps how long any one of them
is allowed to last: the watch really does wait 3.6 s between blinks, but a
README that spends 3.6 s of every loop on a still frame is a README nobody
watches to the end of.
"""

import pathlib
import struct
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
GIF = ROOT / "docs" / "watch.gif"

# The longest any one frame may be held, in ms. See the note above.
MAX_HOLD_MS = 1200


def render(path):
    """Build, run the renderer, and read back what it wrote."""
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(ROOT / "build")],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(ROOT / "build"), "-j"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(ROOT / "build" / "kai_demo"), str(path)], check=True)

    with open(path, "rb") as f:
        width, height, count = struct.unpack("<3I", f.read(12))
        frames, durations = [], []
        for _ in range(count):
            durations.append(struct.unpack("<I", f.read(4))[0])
            frames.append(Image.frombytes("RGB", (width, height),
                                          f.read(width * height * 3)))
    return frames, durations


def main():
    with tempfile.TemporaryDirectory() as tmp:
        frames, durations = render(pathlib.Path(tmp) / "frames.bin")

    durations = [min(ms, MAX_HOLD_MS) for ms in durations]

    # One palette for the whole animation rather than one per frame. The faces
    # are amber on black and nothing else, so there are few enough colours to
    # keep every one of them and hand the GIF exactly what was rendered.
    colours = set()
    for frame in frames:
        found = frame.getcolors(maxcolors=256)
        if found is None:
            sys.exit("a frame holds more colours than a GIF palette does")
        colours |= {colour for _, colour in found}
    if len(colours) > 256:
        sys.exit(f"{len(colours)} colours, more than a GIF palette holds")
    palette = Image.new("P", (1, 1))
    flat = [channel for colour in sorted(colours) for channel in colour]
    palette.putpalette(flat + [0] * (768 - len(flat)))
    frames = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]

    GIF.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(GIF, save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, optimize=True, disposal=1)

    total = sum(durations)
    print(f"{GIF.relative_to(ROOT)}: {len(frames)} frames, "
          f"{len(colours)} colours, {total / 1000:.1f} s, "
          f"{GIF.stat().st_size / 1024:.0f} kB")


if __name__ == "__main__":
    main()
