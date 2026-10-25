#!/usr/bin/env python3
"""Phone screen relay for esp-claw's cap_phone_screen (plan B).

The board forwards the iPhone's AirPlay H.264 stream (raw Annex-B) over TCP. The phone sends
an IDR only when mirroring starts, so this relay keeps ONE ffmpeg decoder running for the
whole session and always holds the latest decoded frame as JPEG.

  TCP  :8091  board -> relay   raw H.264 (SPS/PPS + IDR first, then every access unit)
  HTTP :8090  GET /frame.jpg   latest frame (JPEG, scaled to --width)
              GET /frame.jpg?grid=1  same with a coordinate grid for the vision model: thin
                               lines every 100 units of 0-1000, thick lines at 500
              GET /status      JSON counters
              GET /mark        remember the current (grey, small) frame
              GET /diff        where the screen changed since /mark: bbox + center in 0-1000
                               (status bar ignored); the agent wiggles the pointer between
                               /mark and /diff to find it, as esp32-iphone-agent's app did
              ?slot=NAME       on /mark and /diff: a separate remembered frame, so a long-lived
                               mark (screen before an agent step) survives the short-lived
                               marks of pointer location in between
              GET /still       {"since_ms": how long the screen (below the status bar) has not
                               changed, "frame_age_ms", "w", "h" (size of /frame.jpg)}: lets the
                               agent wait exactly until the screen settles instead of a fixed
                               mark/sleep/diff window
              GET /row?x=&y=   the list row (between separator lines / card edges) around x, y
                               (0-1000 units) in the latest frame: {"top", "bottom", "center"} in
                               0-1000 units, or {"row": false}; lets the agent tap a row's middle
                               instead of the edge the vision model pointed at
              GET /frame.jpg?marks=1  the latest frame with a red numbered tag at the left of every
                               list row (set-of-marks): the vision model names a row by its number
                               instead of reading coordinates off the picture
              GET /rows        the rows numbered by the last /frame.jpg?marks=1: {"rows": [{"n",
                               "top", "bottom", "center"}]} in 0-1000 units
              ?pointer=1       on /diff with near=: the caller nudged the pointer by +x +y since
                               /mark; pick the changed region nearest to near that looks like a
                               moved pointer and report its resting position as "rest"

Needs only the Python standard library and an ffmpeg binary on PATH.
Run:  python3 tools/phone_relay/relay.py [--width 540] [--stream-port 8091] [--http-port 8090]
"""
import argparse
import json
import os
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

JPEG_SOI = b"\xff\xd8"
JPEG_EOI = b"\xff\xd9"
# thin lines every 10% (100 units of 0-1000), thick lines at 50%; magenta reads on dark and light UIs
GRID_FILTER = ("drawgrid=w=iw/10:h=ih/10:t=1:c=magenta@0.55,"
               "drawgrid=w=iw/2:h=ih/2:t=3:c=magenta@0.8")
GRAY_W, GRAY_H = 180, 392      # 1/4 of the iPhone's 720x1568 mirror; enough to see the pointer
BLOCK = 6                      # diff is averaged over BLOCK x BLOCK pixel blocks
BLOCK_THRESHOLD = 12.0         # mean grey-level change of the strongest block to count as a change
NEAR = 4                       # blocks around the strongest one that belong to the same object
NEAR_RADIUS = 500              # /diff?near=x,y search radius, in 0-1000 units of the screen WIDTH
STATUS_BAR = 0.06              # top fraction ignored by /diff (clock, battery)
DEFAULT_SLOT = "default"       # /mark and /diff without ?slot=
# 0-1000 box (x0, y0, x1, y1) also ignored by /diff without near=: the agent parks the pointer in
# the top-left corner, where iOS snaps it onto the nearest Home Screen icon (Clock or Calendar,
# depending on where it came from) and the Clock icon's second hand ticks. Pointer location
# (/diff?near=) still sees it.
PARK_MASK = (0, 0, 520, 200)
# /diff without near= also reports "upper": the fraction of blocks between the status bar and this
# height that changed. Measured 2026-10-25 on saved frame pairs: opening or leaving a page or a
# sheet changed 0.34-0.93 of it, while the keyboard sliding up, typing digits and wheeling a picker
# changed 0-0.02, so the agent can tell a tap that navigated from one that stayed on the page.
UPPER_BOTTOM = 0.45
# Pointer location (/diff?pointer=1). A nudged pointer leaves two lobes of opposite sign in the
# pixel difference: where it was (gone) and where it is now (new), the latter towards +x +y. A
# fading AssistiveTouch button or a hover highlight changes brightness one way only, so it has a
# single lobe; picking the strongest change instead once read the AssistiveTouch button at the top
# centre as the pointer (error 455 units).
CLUSTER_GAP = 2                # blocks: hot blocks closer than this belong to one region
LOBE_MIN = 18                  # grey levels: a pixel this much darker/brighter belongs to a lobe
LOBE_MIN_PIXELS = 4            # fewer pixels than this is no lobe
LOBE_PAD = 1                   # blocks added around a region when measuring its lobes
# stream data keeps arriving but no frame decodes for this long: the decoder lost its reference
# chain (e.g. this relay restarted mid-session; only a re-cast sends a new IDR). Frames and diffs
# are refused then, so the agent stops instead of acting on a stale screen.
STALE_S = 5.0
# /row: a horizontal line (separator) or card edge is a pixel row of the latest frame that is
# nearly uniform across the label column and differs from the rows ROW_LINE_GAP above and below.
# Measured on Settings (540 x 1168): separators are 1 px lines ~8-25 grey levels off white, rows
# are 70 px apart. The vision model's y is often off by 30-80 px (2026-10-25), so taps hit row
# edges and the neighbouring row.
ROW_X0, ROW_X1 = 0.22, 0.87    # fraction of the width scanned (right of the row icons)
ROW_UNIFORM = 30               # max grey range along a line
ROW_LINE_STEP = 5              # grey levels a line must differ from the rows around it
ROW_LINE_GAP = 2               # px between a line and the rows it is compared with
ROW_MIN_PX, ROW_MAX_PX = 36, 160  # a list row is this tall (screenshot pixels)
# /still: a decoded frame counts as a change when this many sampled pixels moved by more than
# CHANGE_LEVEL grey levels (re-encoding noise alone moves a few pixels by a few levels)
CHANGE_LEVEL = 24
CHANGE_PIXELS = 3
CHANGE_STRIDE = 2              # sample every 2nd pixel of every 2nd row


# /frame.jpg?marks=1: tags are drawn with this 5 x 7 bitmap font (the ffmpeg build here has no
# drawtext). Benchmark 2026-10-25 (logs/vision_bench_2026-10-25): asked for the pixel coordinates
# of a list row, deepseek-flash hit the right row 47% of the time, mostly one or two rows too high;
# asked for the number of a tagged row, 100% (72/72).
DIGITS = {
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11111", "00010", "00100", "00010", "00001", "10001", "01110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
}
TAG_SCALE = 3                  # font pixel size: digits are 15 x 21 px
TAG_X = 2                      # tag's left edge, px
TAG_COLOR, TAG_TEXT = (220, 0, 0), (255, 255, 255)


def decode(frame, pix_fmt):
    """The JPEG's pixels as bytes (gray: 1 byte per pixel, rgb24: 3)."""
    return subprocess.run(["ffmpeg", "-loglevel", "error", "-i", "pipe:0", "-f", "rawvideo",
                           "-pix_fmt", pix_fmt, "pipe:1"], input=frame, capture_output=True).stdout


def row_lines(gray, w, h):
    """For each pixel row of the frame: is it a separator line / card edge (see ROW_*)?"""
    x0, x1 = int(w * ROW_X0), int(w * ROW_X1)
    stats = []
    for py in range(h):
        line = gray[py * w + x0:py * w + x1:2]
        stats.append((sum(line) / len(line), max(line) - min(line)))
    lines = [False] * h
    for py in range(ROW_LINE_GAP, h - ROW_LINE_GAP):
        mean, spread = stats[py]
        above, below = stats[py - ROW_LINE_GAP], stats[py + ROW_LINE_GAP]
        lines[py] = (spread <= ROW_UNIFORM and above[1] <= ROW_UNIFORM and below[1] <= ROW_UNIFORM
                     and (abs(mean - above[0]) >= ROW_LINE_STEP or abs(mean - below[0]) >= ROW_LINE_STEP))
    return lines


def find_rows(lines, h):
    """All list rows, top to bottom, as (top, bottom) px: the space between two neighbouring
    separators when it is row-sized. Includes the gaps between cards, which are harmless tags."""
    groups = []  # separators: runs of line pixel rows
    for py, is_line in enumerate(lines):
        if not is_line:
            continue
        if groups and py - groups[-1][1] <= ROW_LINE_GAP:
            groups[-1][1] = py
        else:
            groups.append([py, py])
    rows = []
    for (_, top), (bottom, _) in zip(groups, groups[1:]):
        if ROW_MIN_PX <= bottom - top <= ROW_MAX_PX and (top + bottom) / 2 > h * STATUS_BAR:
            rows.append((top, bottom))
    return rows


def draw_tags(rgb, w, h, rows):
    """Paint a red tag with the row's number (1-based) at the left of each row, in place."""
    def fill(x0, y0, x1, y1, color):
        x0, y0, x1, y1 = max(0, x0), max(0, y0), min(w, x1), min(h, y1)
        line = bytes(color) * (x1 - x0)
        for y in range(y0, y1):
            rgb[(y * w + x0) * 3:(y * w + x1) * 3] = line
    s = TAG_SCALE
    for n, (top, bottom) in enumerate(rows, 1):
        text = str(n)
        tw, th = len(text) * 6 * s + 3 * s, 9 * s
        cy = (top + bottom) // 2
        y0 = cy - th // 2
        fill(TAG_X, y0, TAG_X + tw, y0 + th, TAG_COLOR)
        for i, ch in enumerate(text):
            for r, bits in enumerate(DIGITS[ch]):
                for c, bit in enumerate(bits):
                    if bit == "1":
                        x = TAG_X + 2 * s + i * 6 * s + c * s
                        y = y0 + s + r * s
                        fill(x, y, x + s, y + s, TAG_TEXT)


def jpeg_size(data):
    """(width, height) from a JPEG's SOF marker, or None."""
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            return None
        marker, seg = data[i + 1], int.from_bytes(data[i + 2:i + 4], "big")
        if marker in (0xC0, 0xC1, 0xC2):
            return int.from_bytes(data[i + 7:i + 9], "big"), int.from_bytes(data[i + 5:i + 7], "big")
        i += 2 + seg
    return None


class Relay:
    def __init__(self, width):
        self.width = width
        self.lock = threading.Lock()
        self.frame = None          # latest JPEG bytes
        self.grid_frame = None     # latest JPEG with the coordinate grid
        self.frame_at = 0.0
        self.data_at = 0.0         # when stream bytes last arrived from the board
        self.frames = 0
        self.bytes_in = 0
        self.board = None
        self.sessions = 0
        self.gray = None           # latest small grey frame (bytes, GRAY_W x GRAY_H)
        self.changed_at = 0.0      # when a decoded grey frame last differed from the one before
        self.marked = {}           # slot name -> remembered grey frame
        self.fifo_dir = tempfile.mkdtemp(prefix="phone_relay_")
        self.decoder = None        # current ffmpeg process

    def start_decoder(self):
        """One ffmpeg per board session (its own grey FIFO); a new session replaces the old one."""
        if self.decoder and self.decoder.poll() is None:
            self.decoder.kill()
        fifo = os.path.join(self.fifo_dir, f"gray{self.sessions}.fifo")
        grid_fifo = os.path.join(self.fifo_dir, f"grid{self.sessions}.fifo")
        os.mkfifo(fifo)
        os.mkfifo(grid_fifo)
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            # no "-fflags nobuffer": with it this ffmpeg build emits no frames from piped h264
            "-probesize", "32", "-analyzeduration", "0", "-flags", "low_delay",
            "-threads", "1",  # frame threading would hold back several frames (latency)
            "-f", "h264", "-i", "pipe:0",
            # mjpeg rejects limited-range YUV: convert to full range (yuvj420p)
            "-filter_complex",
            f"[0:v]split=3[a][b][c];[a]scale={self.width}:-2,format=yuvj420p[j];"
            f"[b]scale={GRAY_W}:{GRAY_H},format=gray[g];"
            f"[c]scale={self.width}:-2,{GRID_FILTER},format=yuvj420p[k]",
            "-map", "[j]", "-f", "image2pipe", "-c:v", "mjpeg", "-q:v", "4", "pipe:1",
            "-map", "[g]", "-f", "rawvideo", "-y", fifo,
            # grid lines add a lot of detail: lower quality keeps the model's JPEG ~50 KB
            "-map", "[k]", "-f", "image2pipe", "-c:v", "mjpeg", "-q:v", "7", "-y", grid_fifo,
        ]
        threading.Thread(target=self.read_gray, args=(fifo,), daemon=True).start()
        threading.Thread(target=self.read_grid, args=(grid_fifo,), daemon=True).start()
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        threading.Thread(target=self.read_frames, args=(proc,), daemon=True).start()
        self.decoder = proc
        return proc

    def read_gray(self, fifo):
        size = GRAY_W * GRAY_H
        with open(fifo, "rb") as f:
            while True:
                frame = f.read(size)
                if len(frame) < size:
                    return
                with self.lock:
                    prev = self.gray
                    self.gray = frame
                if prev is None or self.frames_differ(prev, frame):
                    with self.lock:
                        self.changed_at = time.time()

    @staticmethod
    def frames_differ(a, b):
        """Whether two grey frames differ below the status bar (clock ticks do not count)."""
        moved = 0
        for y in range(int(GRAY_H * STATUS_BAR), GRAY_H, CHANGE_STRIDE):
            row = y * GRAY_W
            for x in range(0, GRAY_W, CHANGE_STRIDE):
                if abs(a[row + x] - b[row + x]) > CHANGE_LEVEL:
                    moved += 1
                    if moved >= CHANGE_PIXELS:
                        return True
        return False

    def still(self):
        with self.lock:
            why = self.stalled()
            if why:
                return {"ok": False, "error": why}
            now = time.time()
            frame = self.frame
            result = {"ok": True, "since_ms": round((now - self.changed_at) * 1000),
                      "frame_age_ms": round((now - self.frame_at) * 1000)}
        size = jpeg_size(frame) if frame else None
        if size:
            result["w"], result["h"] = size
        return result

    def row(self, x, y):
        """The list row around (x, y), both 0-1000 units of the latest frame."""
        with self.lock:
            why = self.stalled()
            if why:
                return {"ok": False, "error": why}
            frame = self.frame
        size = jpeg_size(frame) if frame else None
        if not size:
            return {"ok": False, "error": "no frame"}
        w, h = size
        gray = decode(frame, "gray")
        if len(gray) < w * h:
            return {"ok": False, "error": "could not decode the frame"}
        lines = row_lines(gray, w, h)
        is_line = lines.__getitem__

        py = min(h - 1, max(0, round(y / 1000.0 * h)))
        top = next((t for t in range(py, max(-1, py - ROW_MAX_PX), -1) if is_line(t)), None)
        bottom = next((b for b in range(py + 1, min(h, py + ROW_MAX_PX)) if is_line(b)), None)
        k = 1000.0 / h
        if top is None or bottom is None or not ROW_MIN_PX <= bottom - top <= ROW_MAX_PX:
            # the lines found on their own: a point right next to one is on the border of two rows
            # even when the other edge is hidden (e.g. under Settings' floating search field)
            return {"ok": True, "row": False, "top": None if top is None else round(top * k),
                    "bottom": None if bottom is None else round(bottom * k)}
        return {"ok": True, "row": True, "top": round(top * k), "bottom": round(bottom * k),
                "center": round((top + bottom) / 2 * k)}

    def marked_frame(self):
        """/frame.jpg?marks=1: (JPEG with numbered row tags, None) or (None, error). The tagged rows
        are kept for /rows, so a number the model read maps to the frame it saw."""
        with self.lock:
            why = self.stalled()
            frame = self.frame
        size = jpeg_size(frame) if frame else None
        if why or not size:
            return None, why or "no frame yet (is the phone mirroring?)"
        w, h = size
        gray, rgb = decode(frame, "gray"), bytearray(decode(frame, "rgb24"))
        if len(gray) < w * h or len(rgb) < w * h * 3:
            return None, "could not decode the frame"
        rows = find_rows(row_lines(gray, w, h), h)
        draw_tags(rgb, w, h, rows)
        jpeg = subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                               "-s", f"{w}x{h}", "-i", "pipe:0", "-q:v", "4", "-f", "mjpeg", "pipe:1"],
                              input=bytes(rgb), capture_output=True).stdout
        if not jpeg:
            return None, "could not encode the tagged frame"
        k = 1000.0 / h
        with self.lock:
            self.tagged_rows = [{"n": n, "top": round(t * k), "bottom": round(b * k),
                                 "center": round((t + b) / 2 * k)} for n, (t, b) in enumerate(rows, 1)]
        return jpeg, None

    def rows(self):
        with self.lock:
            rows = getattr(self, "tagged_rows", None)
        if rows is None:
            return {"ok": False, "error": "no tagged frame yet (/frame.jpg?marks=1)"}
        return {"ok": True, "rows": rows}

    def stalled(self):
        """Why the latest frame can't be trusted, or None. Call with self.lock held."""
        if not self.frame_at:
            return "no frame decoded yet (is the phone mirroring?)"
        if self.data_at - self.frame_at > STALE_S:
            return ("decoder stalled: stream data arrives but no frame decoded for %.0f s; "
                    "re-cast Screen Mirroring on the phone" % (self.data_at - self.frame_at))
        return None

    def mark(self, slot=DEFAULT_SLOT):
        with self.lock:
            why = self.stalled()
            if why:
                return {"ok": False, "error": why}
            self.marked[slot] = self.gray
        return {"ok": self.gray is not None}

    def diff(self, near=None, radius=NEAR_RADIUS, slot=DEFAULT_SLOT, pointer=False):
        """Where did the screen change since /mark? Block-averaged so that encoder noise (a
        re-quantized frame changes text edges all over the screen) does not drown the pointer,
        which is a soft disc ~8% of the screen width: report the strongest changed region."""
        with self.lock:
            why = self.stalled()
            if why:
                return {"ok": False, "error": why}
            a, b = self.marked.get(slot), self.gray
        if a is None or b is None:
            return {"ok": False, "error": "no grey frames (mirroring?) or /mark not called"}
        # pointer location keeps the status bar: a pointer aimed at a top-row button often rests in it
        y0 = 0 if pointer and near is not None else int(GRAY_H * STATUS_BAR)
        bw, bh = GRAY_W // BLOCK, (GRAY_H - y0) // BLOCK
        blocks = [[0] * bw for _ in range(bh)]
        for by in range(bh):
            for bx in range(bw):
                total = 0
                for y in range(y0 + by * BLOCK, y0 + (by + 1) * BLOCK):
                    row = y * GRAY_W + bx * BLOCK
                    total += sum(abs(a[row + i] - b[row + i]) for i in range(BLOCK))
                blocks[by][bx] = total / (BLOCK * BLOCK)
        cells = [(y, x) for y in range(bh) for x in range(bw)]
        upper = None
        if near is None:  # before the park mask: the pointer is where it tapped, not parked
            rows = [blocks[y] for y in range(bh) if (y0 + (y + 0.5) * BLOCK) / GRAY_H < UPPER_BOTTOM]
            upper = round(sum(v >= BLOCK_THRESHOLD for r in rows for v in r) / max(1, len(rows) * bw), 2)
        if near is None:
            mx0, my0, mx1, my1 = PARK_MASK
            for y, x in cells:
                cx = (x + 0.5) * BLOCK / GRAY_W * 1000.0
                cy = (y0 + (y + 0.5) * BLOCK) / GRAY_H * 1000.0
                if mx0 <= cx < mx1 and my0 <= cy < my1:
                    blocks[y][x] = 0.0
        all_peak = max(blocks[y][x] for y, x in cells)
        if near is not None:
            # only look around where the caller expects the pointer: hover effects elsewhere
            # (e.g. iOS tab-bar highlights) can change more pixels than the pointer itself
            hx = near[0] / 1000.0 * GRAY_W / BLOCK
            hy = (near[1] / 1000.0 * GRAY_H - y0) / BLOCK
            r = radius / 1000.0 * GRAY_W / BLOCK  # blocks are square in pixels: one radius for both axes
            cells = [(y, x) for y, x in cells if (x + 0.5 - hx) ** 2 + (y + 0.5 - hy) ** 2 <= r * r]
        peak = max((blocks[y][x] for y, x in cells), default=0.0)
        if all_peak < BLOCK_THRESHOLD or peak < BLOCK_THRESHOLD:
            return {"ok": True, "changed": 0 if all_peak < BLOCK_THRESHOLD else 1,
                    "peak": round(peak, 1), "all_peak": round(all_peak, 1), "upper": upper}
        if pointer and near is not None:
            return self.locate_pointer(a, b, blocks, cells, near, y0)
        # blocks at >= half the peak, then keep the ones near the strongest block
        py, px = max(cells, key=lambda p: blocks[p[0]][p[1]])
        hot = [(y, x) for y in range(bh) for x in range(bw)
               if blocks[y][x] >= peak / 2 and abs(y - py) <= NEAR and abs(x - px) <= NEAR]
        all_hot = sum(1 for y in range(bh) for x in range(bw) if blocks[y][x] >= peak / 2)
        xs = [x for _, x in hot]
        ys = [y for y, _ in hot]
        k = 1000.0
        to_x = lambda bx: round(bx * BLOCK / GRAY_W * k)
        to_y = lambda by: round((y0 + by * BLOCK) / GRAY_H * k)
        return {
            "ok": True, "changed": all_hot, "near": len(hot), "peak": round(peak, 1), "upper": upper,
            "bbox": [to_x(min(xs)), to_y(min(ys)), to_x(max(xs) + 1), to_y(max(ys) + 1)],
            "center": [round((sum(xs) / len(xs) + 0.5) * BLOCK / GRAY_W * k),
                       round((y0 + (sum(ys) / len(ys) + 0.5) * BLOCK) / GRAY_H * k)],
        }

    def locate_pointer(self, a, b, blocks, cells, near, y0):
        """/diff?pointer=1: split the changed blocks around near into regions, prefer the ones
        that look like a nudged pointer (two opposite lobes, the newer one towards +x +y), then
        the nearest; report that region like /diff plus the pointer's resting position."""
        hot = {(y, x) for y, x in cells if blocks[y][x] >= BLOCK_THRESHOLD}
        regions = []
        while hot:
            todo = [hot.pop()]
            region = list(todo)
            while todo:
                cy, cx = todo.pop()
                close = [p for p in hot if abs(p[0] - cy) <= CLUSTER_GAP and abs(p[1] - cx) <= CLUSTER_GAP]
                for p in close:
                    hot.discard(p)
                todo.extend(close)
                region.extend(close)
            regions.append(region)
        hx = near[0] / 1000.0 * GRAY_W / BLOCK
        hy = (near[1] / 1000.0 * GRAY_H - y0) / BLOCK
        candidates = []
        for region in regions:
            lobes = self.pointer_lobes(a, b, region, y0)
            dist = min(((x + 0.5 - hx) ** 2 + (y + 0.5 - hy) ** 2) ** 0.5 for y, x in region)
            candidates.append((lobes is None, dist, region, lobes))
        candidates.sort(key=lambda c: (c[0], c[1]))
        _, dist, region, lobes = candidates[0]
        peak = max(blocks[y][x] for y, x in region)
        core = [(y, x) for y, x in region if blocks[y][x] >= peak / 2]
        xs = [x for _, x in core]
        ys = [y for y, _ in core]
        to_x = lambda bx: round(bx * BLOCK / GRAY_W * 1000.0)
        to_y = lambda by: round((y0 + by * BLOCK) / GRAY_H * 1000.0)
        result = {
            "ok": True, "changed": len(core), "near": len(core), "peak": round(peak, 1),
            "regions": len(regions), "lobes": lobes is not None,
            "bbox": [to_x(min(xs)), to_y(min(ys)), to_x(max(xs) + 1), to_y(max(ys) + 1)],
            "center": [round((sum(xs) / len(xs) + 0.5) * BLOCK / GRAY_W * 1000.0),
                       round((y0 + (sum(ys) / len(ys) + 0.5) * BLOCK) / GRAY_H * 1000.0)],
        }
        if lobes:
            (rx, ry), (mx, my) = lobes
            result["rest"] = [round(rx / GRAY_W * 1000.0), round(ry / GRAY_H * 1000.0)]
            result["moved"] = [round(mx / GRAY_W * 1000.0), round(my / GRAY_H * 1000.0)]
        return result

    def pointer_lobes(self, a, b, region, y0):
        """((rest x, y), (moved x, y)) in grey-frame pixels when the region holds a pointer that
        moved towards +x +y between frame a and frame b, else None. The status bar is included
        here: a pointer on a top-row button reaches into it."""
        bys = [y for y, _ in region]
        bxs = [x for _, x in region]
        py0 = max(0, y0 + (min(bys) - LOBE_PAD) * BLOCK)
        py1 = min(GRAY_H, y0 + (max(bys) + 1 + LOBE_PAD) * BLOCK)
        px0 = max(0, (min(bxs) - LOBE_PAD) * BLOCK)
        px1 = min(GRAY_W, (max(bxs) + 1 + LOBE_PAD) * BLOCK)
        sums = {1: [0.0, 0.0, 0.0, 0], -1: [0.0, 0.0, 0.0, 0]}  # sign -> weight, wx, wy, pixels
        for y in range(py0, py1):
            row = y * GRAY_W
            for x in range(px0, px1):
                s = a[row + x] - b[row + x]
                if abs(s) < LOBE_MIN:
                    continue
                acc = sums[1 if s > 0 else -1]
                w = abs(s)
                acc[0] += w
                acc[1] += w * (x + 0.5)
                acc[2] += w * (y + 0.5)
                acc[3] += 1
        if any(acc[3] < LOBE_MIN_PIXELS for acc in sums.values()):
            return None
        p, n = ((acc[1] / acc[0], acc[2] / acc[0]) for acc in (sums[1], sums[-1]))
        rest, moved = (p, n) if p[0] + p[1] < n[0] + n[1] else (n, p)
        if moved[0] - rest[0] < 1 or moved[1] - rest[1] < 1:
            return None  # not a +x +y move: something else changed
        return rest, moved

    def read_frames(self, proc):
        self.read_jpegs(proc.stdout, "frame")

    def read_grid(self, fifo):
        with open(fifo, "rb") as f:
            self.read_jpegs(f, "grid_frame")

    def read_jpegs(self, stream, attr):
        """Split an MJPEG byte stream into JPEGs; keep the latest in self.<attr>."""
        buf = b""
        while True:
            chunk = stream.read1(65536)
            if not chunk:
                return
            buf += chunk
            while True:
                start = buf.find(JPEG_SOI)
                end = buf.find(JPEG_EOI, start + 2) if start >= 0 else -1
                if end < 0:
                    break
                with self.lock:
                    setattr(self, attr, buf[start:end + 2])
                    if attr == "frame":
                        self.frame_at = time.time()
                        self.frames += 1
                buf = buf[end + 2:]

    def serve_board(self, conn, addr):
        """One board connection = one decoder (the board replays SPS/PPS + IDR on connect)."""
        print(f"board connected from {addr[0]}", flush=True)
        with self.lock:
            self.board = addr[0]
            self.sessions += 1
        try:
            proc = self.start_decoder()
        except Exception as e:  # close the link so the board reconnects instead of hanging
            print(f"decoder start failed: {e}", flush=True)
            conn.close()
            return
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                with self.lock:
                    self.bytes_in += len(data)
                    self.data_at = time.time()
                proc.stdin.write(data)
                proc.stdin.flush()
        except (BrokenPipeError, ConnectionResetError) as e:
            print(f"stream error: {e}", flush=True)
        finally:
            conn.close()
            proc.stdin.close()
            proc.wait(timeout=5)
            print("board disconnected", flush=True)

    def status(self):
        with self.lock:
            age = time.time() - self.frame_at if self.frame_at else None
            return {
                "board": self.board, "sessions": self.sessions, "bytes_in": self.bytes_in,
                "frames_decoded": self.frames, "last_frame_age_s": None if age is None else round(age, 2),
                "frame_bytes": len(self.frame) if self.frame else 0,
                "stalled": self.stalled(),
            }


def run_stream_server(relay, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print(f"stream: listening on :{port}", flush=True)
    while True:
        conn, addr = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=relay.serve_board, args=(conn, addr), daemon=True).start()


def make_handler(relay):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/frame.jpg"):
                if "marks=1" in self.path:
                    frame, why = relay.marked_frame()
                else:
                    with relay.lock:
                        frame = relay.grid_frame if "grid=1" in self.path else relay.frame
                        why = relay.stalled()
                if why or not frame:
                    self.send_error(503, why or "no frame yet (is the phone mirroring?)")
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(frame)))
                self.end_headers()
                self.wfile.write(frame)
            elif self.path.startswith(("/status", "/mark", "/diff", "/still", "/row")):  # /row, /rows
                route, _, query = self.path.partition("?")
                q = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
                slot = q.get("slot", DEFAULT_SLOT)
                if route == "/diff":
                    near = [float(v) for v in q["near"].split(",")] if "near" in q else None
                    result = relay.diff(near, float(q.get("radius", NEAR_RADIUS)), slot,
                                        pointer=q.get("pointer") == "1")
                elif route == "/mark":
                    result = relay.mark(slot)
                elif route == "/still":
                    result = relay.still()
                elif route == "/rows":
                    result = relay.rows()
                elif route == "/row":
                    result = relay.row(float(q.get("x", 500)), float(q.get("y", 500)))
                else:
                    result = relay.status()
                body = json.dumps(result).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_error(404)

        def log_message(self, fmt, *args):
            stamp = time.strftime("%H:%M:%S") + ".%03d" % (time.time() % 1 * 1000)
            print(f"{stamp} http: {self.address_string()} {fmt % args}", flush=True)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--width", type=int, default=540, help="JPEG width served to the board (default 540)")
    ap.add_argument("--stream-port", type=int, default=8091)
    ap.add_argument("--http-port", type=int, default=8090)
    args = ap.parse_args()
    relay = Relay(args.width)
    threading.Thread(target=run_stream_server, args=(relay, args.stream_port), daemon=True).start()
    print(f"http: listening on :{args.http_port}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.http_port), make_handler(relay)).serve_forever()


if __name__ == "__main__":
    main()
