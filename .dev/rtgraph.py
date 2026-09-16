#!/usr/bin/env python3
"""Render-target aliasing analyser for KytyPS5 emulator logs.

Consumes the PRODUCE / CONSUME / DOWNLOAD / EDGE / COMPUTE IMG probe lines emitted
by the graphics-shader-fixes debug build and answers questions a raw log cannot:

  overlap    which distinct surfaces share guest memory, and by how many bytes
  stomp      which guest->image upload read a range that a different-format
             image had written more recently
  orphan     which surfaces are sampled as textures without ever being produced
             at that shape -- the "a pass got handed the wrong image" class
  downloads  which image->guest readbacks overwrite each other's ranges
  predict    the on-screen artefact each cross-format misread should produce
  cause      why each 1080p image was re-uploaded, and which buffer write
             surrendered its GPU ownership
  edges      the texture -> render target graph

Usage:
    python .dev/rtgraph.py LOG [--tail-bytes N] [--limit N] [--section a,b,c]
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import defaultdict

# VkFormat values that appear in these logs, with bytes per pixel. Anything not
# listed prints as its raw number.
VK_FORMATS = {
    0: ("UNDEFINED", 0),
    13: ("R8_UINT", 1),
    15: ("R8_SRGB", 1),
    37: ("R8G8B8A8_UNORM", 4),
    43: ("R8G8B8A8_SRGB", 4),
    44: ("B8G8R8A8_UNORM", 4),
    50: ("B8G8R8A8_SRGB", 4),
    64: ("A2B10G10R10_UNORM_PACK32", 4),
    70: ("R16_UNORM", 2),
    76: ("R16_SFLOAT", 2),
    77: ("R16G16_UNORM", 4),
    83: ("R16G16_SFLOAT", 4),
    91: ("R16G16B16A16_UNORM", 8),
    95: ("R16G16B16A16_UINT", 8),
    97: ("R16G16B16A16_SFLOAT", 8),
    100: ("R32_SFLOAT", 4),
    107: ("R32G32B32A32_UINT", 16),
    109: ("R32G32B32A32_SFLOAT", 16),
    122: ("B10G11R11_UFLOAT_PACK32", 4),
    124: ("D16_UNORM", 2),
    126: ("D32_SFLOAT", 4),
    129: ("D24_UNORM_S8_UINT", 4),
    130: ("D32_SFLOAT_S8_UINT", 5),
    133: ("BC1_RGBA_UNORM_BLOCK", 0),
    138: ("BC3_SRGB_BLOCK", 0),
    141: ("BC5_UNORM_BLOCK", 0),
}


def fmt_name(value):
    entry = VK_FORMATS.get(value)
    return "%s(%d)" % (entry[0], value) if entry else "fmt%d" % value


def fmt_bpp(value):
    entry = VK_FORMATS.get(value)
    return entry[1] if entry else 0


RE_PRODUCE = re.compile(
    r"^PRODUCE: frame=(\d+) seq=(\d+) addr=0x([0-9a-f]+) size=0x([0-9a-f]+) "
    r"(\d+)x(\d+) fmt=(\d+) guest_fmt=(\d+) mips=(\d+) layers=(\d+)")
RE_CONSUME = re.compile(
    r"^CONSUME: frame=(\d+) seq=(\d+) addr=0x([0-9a-f]+) size=0x([0-9a-f]+) "
    r"(\d+)x(\d+) fmt=(\d+) guest_fmt=(\d+) mips=(\d+) layers=(\d+) binding=(\d+)")
RE_DOWNLOAD = re.compile(
    r"^DOWNLOAD: image->guest addr=0x([0-9a-f]+) (\d+)x(\d+) fmt=(\d+) "
    r"range=0x([0-9a-f]+)-0x([0-9a-f]+) size=0x([0-9a-f]+) outcome=(\S+)")
RE_EDGE = re.compile(
    r"^EDGE: tex=0x([0-9a-f]+) (\d+)x(\d+) fmt=(\d+) -> rt=0x([0-9a-f]+) "
    r"(\d+)x(\d+) fmt=(\d+)")
RE_REFRESH = re.compile(
    r"^REFRESH UPLOAD: frame=(\d+) seq=(\d+) addr=0x([0-9a-f]+) size=0x([0-9a-f]+) "
    r"(\d+)x(\d+) fmt=(\d+) buffer_modified=(\d) cpu_dirty=(\d) maybe_cpu=(\d) "
    r"gpu_modified=(\d)")
RE_INVALIDATE = re.compile(
    r"^IMAGE INVALIDATE: frame=(\d+) seq=(\d+) image=0x([0-9a-f]+) size=0x([0-9a-f]+) "
    r"(\d+)x(\d+) fmt=(\d+) by_buffer=0x([0-9a-f]+) size=0x([0-9a-f]+) "
    r"overlap=0x([0-9a-f]+) was_gpu_modified=(\d)")
RE_COMPUTE = re.compile(
    r"^COMPUTE IMG: dispatch=(\d+) shader=0x([0-9a-f]+) slot=(\d+) "
    r"addr=0x([0-9a-f]+) (\d+)x(\d+) fmt=(-?\d+) mip=\d+\+\d+ layer=\d+\+\d+ "
    r"access=(\S+) storage=(\d)")

PAGE = 1 << 16


class Surface(object):
    """One (address, extent, format) shape. A guest address alone is not an
    identity: the game recycles one allocation for surfaces of different extent
    and format, so keying on the address merges unrelated resources."""

    __slots__ = ("addr", "width", "height", "fmt", "size", "produced",
                 "consumed", "first_seq", "last_seq", "frames")

    def __init__(self, addr, width, height, fmt, size):
        self.addr = addr
        self.width = width
        self.height = height
        self.fmt = fmt
        self.size = size
        self.produced = 0
        self.consumed = 0
        self.first_seq = None
        self.last_seq = None
        self.frames = set()

    @property
    def key(self):
        return (self.addr, self.width, self.height, self.fmt)

    @property
    def end(self):
        return self.addr + self.size

    def touch(self, seq, frame):
        if self.first_seq is None:
            self.first_seq = seq
        self.last_seq = seq
        self.frames.add(frame)

    def label(self):
        return "0x%016x+0x%x %dx%d %s" % (self.addr, self.size, self.width,
                                          self.height, fmt_name(self.fmt))


class Log(object):
    def __init__(self):
        self.surfaces = {}
        self.events = []
        self.downloads = []
        self.edges = defaultdict(int)
        self.compute_writes = defaultdict(set)
        self.compute_reads = defaultdict(set)
        self.refreshes = []
        self.invalidations = []

    def surface(self, addr, width, height, fmt, size):
        key = (addr, width, height, fmt)
        found = self.surfaces.get(key)
        if found is None:
            found = Surface(addr, width, height, fmt, size)
            self.surfaces[key] = found
        elif size > found.size:
            # Mip chains report a larger size on some acquisitions; keep the
            # widest range, since that is what can collide with a neighbour.
            found.size = size
        return found


def parse(stream):
    log = Log()
    for line in stream:
        head = line[:8]
        if head.startswith("PRODUCE:"):
            m = RE_PRODUCE.match(line)
            if not m:
                continue
            frame, seq = int(m.group(1)), int(m.group(2))
            surface = log.surface(int(m.group(3), 16), int(m.group(5)),
                                  int(m.group(6)), int(m.group(7)),
                                  int(m.group(4), 16))
            surface.produced += 1
            surface.touch(seq, frame)
            log.events.append((seq, frame, "produce", surface))
        elif head.startswith("CONSUME:"):
            m = RE_CONSUME.match(line)
            if not m:
                continue
            frame, seq = int(m.group(1)), int(m.group(2))
            surface = log.surface(int(m.group(3), 16), int(m.group(5)),
                                  int(m.group(6)), int(m.group(7)),
                                  int(m.group(4), 16))
            surface.consumed += 1
            surface.touch(seq, frame)
            log.events.append((seq, frame, "consume", surface))
        elif head.startswith("DOWNLOAD"):
            m = RE_DOWNLOAD.match(line)
            if not m:
                continue
            log.downloads.append({
                "addr": int(m.group(1), 16),
                "width": int(m.group(2)),
                "height": int(m.group(3)),
                "fmt": int(m.group(4)),
                "start": int(m.group(5), 16),
                "end": int(m.group(6), 16),
                "size": int(m.group(7), 16),
                "outcome": m.group(8),
            })
        elif head.startswith("REFRESH "):
            m = RE_REFRESH.match(line)
            if not m:
                continue
            log.refreshes.append({
                "frame": int(m.group(1)), "seq": int(m.group(2)),
                "addr": int(m.group(3), 16), "size": int(m.group(4), 16),
                "width": int(m.group(5)), "height": int(m.group(6)),
                "fmt": int(m.group(7)), "buffer_modified": m.group(8) == "1",
                "cpu_dirty": m.group(9) == "1", "maybe_cpu": m.group(10) == "1",
                "gpu_modified": m.group(11) == "1",
            })
        elif head.startswith("IMAGE IN"):
            m = RE_INVALIDATE.match(line)
            if not m:
                continue
            log.invalidations.append({
                "frame": int(m.group(1)), "seq": int(m.group(2)),
                "addr": int(m.group(3), 16), "size": int(m.group(4), 16),
                "width": int(m.group(5)), "height": int(m.group(6)),
                "fmt": int(m.group(7)), "buffer": int(m.group(8), 16),
                "buffer_size": int(m.group(9), 16), "overlap": int(m.group(10), 16),
                "was_gpu_modified": m.group(11) == "1",
            })
        elif head.startswith("EDGE:"):
            m = RE_EDGE.match(line)
            if not m:
                continue
            tex = (int(m.group(1), 16), int(m.group(2)), int(m.group(3)),
                   int(m.group(4)))
            rt = (int(m.group(5), 16), int(m.group(6)), int(m.group(7)),
                  int(m.group(8)))
            log.edges[(tex, rt)] += 1
        elif head.startswith("COMPUTE "):
            m = RE_COMPUTE.match(line)
            if not m:
                continue
            key = (int(m.group(4), 16), int(m.group(5)), int(m.group(6)),
                   int(m.group(7)))
            shader = int(m.group(2), 16)
            access = m.group(8)
            if "write" in access or access == "atomic":
                log.compute_writes[key].add(shader)
            if "read" in access:
                log.compute_reads[key].add(shader)
    return log


def overlaps(a, b):
    return max(0, min(a.end, b.end) - max(a.addr, b.addr))


def report_overlap(log, limit):
    print("=" * 96)
    print("OVERLAP  distinct surfaces sharing guest memory")
    print("=" * 96)
    ordered = sorted(log.surfaces.values(), key=lambda s: s.addr)
    pairs = []
    for i, a in enumerate(ordered):
        for b in ordered[i + 1:]:
            if b.addr >= a.end:
                break
            shared = overlaps(a, b)
            if shared:
                pairs.append((shared, a, b))
    pairs.sort(key=lambda item: -item[0])
    if not pairs:
        print("  none")
        return
    print("  %d overlapping pairs; worst %d:" % (len(pairs), limit))
    print("")
    for shared, a, b in pairs[:limit]:
        kind = "same-format" if a.fmt == b.fmt else "CROSS-FORMAT"
        pct = 100.0 * shared / min(a.size, b.size)
        print("  %8.2f MiB (%5.1f%% of smaller)  %s" %
              (shared / 1048576.0, pct, kind))
        print("      A %s  produced=%d consumed=%d" %
              (a.label(), a.produced, a.consumed))
        print("      B %s  produced=%d consumed=%d" %
              (b.label(), b.produced, b.consumed))
    print("")


def report_stomp(log, limit):
    """A guest->image upload only makes sense if the bytes under it were last
    written by the same surface. If a different-shape surface produced into that
    range more recently, the upload detiles another image's pixels."""
    print("=" * 96)
    print("STOMP  uploads that read a range another shape wrote more recently")
    print("=" * 96)
    last_producer = {}
    hits = {}
    for seq, frame, kind, surface in sorted(log.events, key=lambda e: e[0]):
        first_page = surface.addr // PAGE
        last_page = (surface.end - 1) // PAGE
        if kind == "produce":
            for index in range(first_page, last_page + 1):
                last_producer[index] = surface
            continue
        culprits = defaultdict(int)
        for index in range(first_page, last_page + 1):
            other = last_producer.get(index)
            if other is None or other.key == surface.key:
                continue
            culprits[other.key] += PAGE
        for other_key, clobbered in culprits.items():
            other = log.surfaces[other_key]
            if other.fmt == surface.fmt and other.width == surface.width:
                continue  # same shape at a different base: benign reuse
            key = (surface.key, other_key)
            record = hits.get(key)
            if record is None:
                record = {"victim": surface, "culprit": other, "count": 0,
                          "bytes": 0, "frames": set()}
                hits[key] = record
            record["count"] += 1
            record["bytes"] = max(record["bytes"], clobbered)
            record["frames"].add(frame)
    if not hits:
        print("  none")
        return
    ranked = sorted(hits.values(), key=lambda r: -r["count"])
    print("  %d distinct victim/culprit shape pairs; worst %d:" %
          (len(ranked), limit))
    print("")
    for record in ranked[:limit]:
        victim = record["victim"]
        culprit = record["culprit"]
        print("  x%-6d over %d frames, %.2f MiB clobbered" %
              (record["count"], len(record["frames"]),
               record["bytes"] / 1048576.0))
        print("      consumed           %s" % victim.label())
        print("      last written by    %s" % culprit.label())
        bpp_v, bpp_c = fmt_bpp(victim.fmt), fmt_bpp(culprit.fmt)
        if bpp_v and bpp_c and bpp_v != bpp_c:
            print("      %d bpp -> %d bpp: one writer row covers %.2f reader "
                  "rows (striping)" % (bpp_c, bpp_v, float(bpp_c) / bpp_v))
    print("")


def report_orphan(log, limit):
    print("=" * 96)
    print("ORPHAN  surfaces sampled as textures that nothing produced at that shape")
    print("=" * 96)
    orphans = [s for s in log.surfaces.values() if s.consumed and not s.produced]
    compute_only = [s for s in orphans if log.compute_writes.get(s.key)]
    compute_keys = set(s.key for s in compute_only)
    true_orphans = [s for s in orphans if s.key not in compute_keys]
    print("  %d consumed-without-produce; %d are compute-written (fine), "
          "%d have no writer at all" %
          (len(orphans), len(compute_only), len(true_orphans)))
    print("")
    for surface in sorted(true_orphans, key=lambda s: -s.consumed)[:limit]:
        print("  consumed x%-5d %s" % (surface.consumed, surface.label()))
        alias = [o for o in log.surfaces.values()
                 if o.key != surface.key and o.produced and overlaps(surface, o)]
        for other in sorted(alias, key=lambda o: -overlaps(surface, o))[:3]:
            print("      aliases producer %s (%.2f MiB)" %
                  (other.label(), overlaps(surface, other) / 1048576.0))
    print("")


def report_downloads(log):
    print("=" * 96)
    print("DOWNLOAD  image -> guest readbacks and the ranges they collide with")
    print("=" * 96)
    ordered = sorted(log.downloads, key=lambda d: d["start"])
    for entry in ordered:
        print("  0x%012x-0x%012x %dx%d %s outcome=%s" %
              (entry["start"], entry["end"], entry["width"], entry["height"],
               fmt_name(entry["fmt"]), entry["outcome"]))
        for other in ordered:
            if other is entry:
                continue
            shared = (min(entry["end"], other["end"]) -
                      max(entry["start"], other["start"]))
            if shared > 0:
                print("      collides with 0x%012x %s over %.2f MiB" %
                      (other["start"], fmt_name(other["fmt"]),
                       shared / 1048576.0))
    print("")


def unpack_small_float(bits, mantissa_bits):
    exponent = bits >> mantissa_bits
    mantissa = bits & ((1 << mantissa_bits) - 1)
    if exponent == 0:
        return mantissa / float(1 << mantissa_bits) * 2.0 ** -14
    if exponent == 31:
        return float("nan")
    return (1.0 + mantissa / float(1 << mantissa_bits)) * 2.0 ** (exponent - 15)


def decode_as(fmt, word):
    """Decode one 32-bit guest word as the reader's format. Only the packed
    formats that actually collide in these logs are modelled."""
    if fmt == 122:      # B10G11R11_UFLOAT_PACK32
        return (unpack_small_float(word & 0x7ff, 6),
                unpack_small_float((word >> 11) & 0x7ff, 6),
                unpack_small_float((word >> 22) & 0x3ff, 5))
    if fmt == 64:       # A2B10G10R10_UNORM_PACK32
        return ((word & 0x3ff) / 1023.0, ((word >> 10) & 0x3ff) / 1023.0,
                ((word >> 20) & 0x3ff) / 1023.0)
    if fmt in (37, 43):  # R8G8B8A8
        return ((word & 0xff) / 255.0, ((word >> 8) & 0xff) / 255.0,
                ((word >> 16) & 0xff) / 255.0)
    if fmt in (44, 50):  # B8G8R8A8
        return (((word >> 16) & 0xff) / 255.0, ((word >> 8) & 0xff) / 255.0,
                (word & 0xff) / 255.0)
    return None


def sample_words(fmt):
    """Representative 32-bit words a writer of this format leaves in memory."""
    words = []
    if fmt == 97:       # R16G16B16A16_SFLOAT: one word is the R,G half
        for r, g in ((0.05, 0.06), (0.2, 0.25), (0.4, 0.4), (1.5, 1.8), (8.0, 9.0)):
            words.append(half_bits(r) | (half_bits(g) << 16))
    elif fmt in (44, 50, 37, 43):
        for b, g, r in ((12, 14, 16), (30, 35, 40), (90, 100, 110), (200, 205, 210)):
            words.append(b | (g << 8) | (r << 16) | (255 << 24))
    elif fmt == 64:
        for v in (60, 200, 500, 900):
            words.append(v | (v << 10) | (v << 20) | (3 << 30))
    elif fmt == 13:     # R8_UINT: four packed single-channel samples
        for v in (8, 40, 120, 220):
            words.append(v | (v << 8) | (v << 16) | (v << 24))
    return words


def half_bits(value):
    packed = struct.unpack("<I", struct.pack("<f", value))[0]
    if value == 0.0:
        return 0
    sign = (packed >> 16) & 0x8000
    exponent = ((packed >> 23) & 0xff) - 127 + 15
    mantissa = (packed >> 13) & 0x3ff
    if exponent <= 0:
        return sign
    if exponent >= 31:
        return sign | 0x7c00
    return sign | (exponent << 10) | mantissa


def report_predict(log, limit):
    """Turn each stomp pair into the artefact it should produce on screen, so a
    screenshot can confirm or refute the pairing without a capture tool."""
    print("=" * 96)
    print("PREDICT  what each cross-format misread looks like on screen")
    print("=" * 96)
    pairs = set()
    ordered = sorted(log.surfaces.values(), key=lambda s: s.addr)
    for i, a in enumerate(ordered):
        for b in ordered[i + 1:]:
            if b.addr >= a.end:
                break
            if a.fmt == b.fmt or not overlaps(a, b):
                continue
            reader, writer = (a, b) if a.consumed else (b, a)
            if not reader.consumed or not writer.produced:
                continue
            pairs.add((reader.key, writer.key))
    shown = 0
    for reader_key, writer_key in sorted(pairs):
        reader = log.surfaces[reader_key]
        writer = log.surfaces[writer_key]
        words = sample_words(writer.fmt)
        if not words or decode_as(reader.fmt, 0) is None:
            continue
        shown += 1
        if shown > limit:
            break
        print("  reader %s" % reader.label())
        print("  writer %s" % writer.label())
        bpp_r, bpp_w = fmt_bpp(reader.fmt), fmt_bpp(writer.fmt)
        if bpp_r and bpp_w:
            print("      stride: one writer row spans %.2f reader rows -> "
                  "horizontal banding at that period" % (float(bpp_w) / bpp_r))
        for word in words:
            rgb = decode_as(reader.fmt, word)
            dominant = "rgb"[max(range(3), key=lambda i: -1.0 if rgb[i] != rgb[i]
                                 else rgb[i])]
            print("      0x%08x -> R=%-10.4g G=%-10.4g B=%-10.4g  dominant=%s" %
                  (word, rgb[0], rgb[1], rgb[2], dominant))
        print("")
    if shown == 0:
        print("  no modelled cross-format pair")
    print("")


def report_cause(log, limit):
    """Why each 1080p image was re-uploaded from guest memory, and which buffer
    write surrendered its GPU ownership. This is the pairing that decides
    whether a frame shows the compute result or whatever is in RAM."""
    print("=" * 96)
    print("CAUSE  uploads and the invalidations that forced them")
    print("=" * 96)
    if not log.refreshes and not log.invalidations:
        print("  no REFRESH UPLOAD / IMAGE INVALIDATE probes in this log")
        print("")
        return
    reasons = defaultdict(int)
    for entry in log.refreshes:
        flags = []
        if entry["buffer_modified"]:
            flags.append("buffer_modified")
        if entry["cpu_dirty"]:
            flags.append("cpu_dirty")
        if entry["maybe_cpu"]:
            flags.append("maybe_cpu")
        reasons[(entry["addr"], entry["fmt"], "+".join(flags) or "none")] += 1
    print("  %d uploads of 1080p images, by reason:" % len(log.refreshes))
    for (addr, fmt, why), count in sorted(reasons.items(), key=lambda kv: -kv[1])[:limit]:
        print("      x%-6d 0x%012x %-28s %s" % (count, addr, fmt_name(fmt), why))
    print("")
    culprits = defaultdict(lambda: {"count": 0, "discarded": 0, "written": 0,
                                    "gpu_owned": 0})
    for entry in log.invalidations:
        key = (entry["addr"], entry["fmt"], entry["buffer"], entry["buffer_size"])
        record = culprits[key]
        record["count"] += 1
        record["discarded"] = entry["size"]
        record["written"] = entry["buffer_size"]
        if entry["was_gpu_modified"]:
            record["gpu_owned"] += 1
    print("  %d invalidations of 1080p images; worst offenders by leverage "
          "(image bytes discarded per buffer byte written):" % len(log.invalidations))
    ranked = sorted(culprits.items(),
                    key=lambda kv: -(float(kv[1]["discarded"]) /
                                     max(1, kv[1]["written"])))
    for (addr, fmt, buffer_addr, buffer_size), record in ranked[:limit]:
        leverage = float(record["discarded"]) / max(1, record["written"])
        print("      x%-6d image 0x%012x %s" % (record["count"], addr, fmt_name(fmt)))
        print("              buffer 0x%012x wrote %d bytes, discarded %.2f MiB "
              "(%.0fx), %d times while GPU-owned" %
              (buffer_addr, record["written"], record["discarded"] / 1048576.0,
               leverage, record["gpu_owned"]))
    print("")


def report_timeline(log, addr, frames_wanted):
    """Per-frame ordering around one guest range. The stomp report says a race
    exists; this says which pass wins it on each individual frame."""
    print("=" * 96)
    print("TIMELINE  events touching 0x%012x, in submission order" % addr)
    print("=" * 96)
    focus = [s for s in log.surfaces.values() if s.addr <= addr < s.end]
    if not focus:
        print("  no surface covers that address")
        return
    lo = min(s.addr for s in focus)
    hi = max(s.end for s in focus)
    print("  range 0x%012x-0x%012x, %d surfaces claim it:" % (lo, hi, len(focus)))
    for surface in sorted(focus, key=lambda s: -s.consumed):
        print("      %s produced=%d consumed=%d" %
              (surface.label(), surface.produced, surface.consumed))
    print("")
    touching = [e for e in sorted(log.events, key=lambda e: e[0])
                if e[3].addr < hi and lo < e[3].end]
    seen_frames = []
    for _, frame, _, _ in touching:
        if frame not in seen_frames:
            seen_frames.append(frame)
    for frame in seen_frames[:frames_wanted]:
        print("  frame %d" % frame)
        for seq, event_frame, kind, surface in touching:
            if event_frame != frame:
                continue
            mark = "W" if kind == "produce" else "r"
            print("      seq %-8d %s %s" % (seq, mark, surface.label()))
    print("")


def report_edges(log, limit):
    print("=" * 96)
    print("EDGES  texture -> render target, most frequent first")
    print("=" * 96)
    for (tex, rt), count in sorted(log.edges.items(), key=lambda kv: -kv[1])[:limit]:
        print("  x%-5d 0x%012x %dx%d %s  ->  0x%012x %dx%d %s" %
              (count, tex[0], tex[1], tex[2], fmt_name(tex[3]),
               rt[0], rt[1], rt[2], fmt_name(rt[3])))
    print("")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("--tail-bytes", type=int, default=0,
                        help="read only the last N bytes; logs reach gigabytes")
    parser.add_argument("--limit", type=int, default=15)
    parser.add_argument("--section",
                        default="overlap,stomp,orphan,predict,cause,downloads,edges")
    parser.add_argument("--timeline", default=None,
                        help="guest address to trace per frame, e.g. 0x30611f0000")
    parser.add_argument("--timeline-frames", type=int, default=6)
    args = parser.parse_args()

    handle = open(args.log, "r", encoding="utf-8", errors="replace")
    try:
        if args.tail_bytes:
            handle.seek(0, 2)
            handle.seek(max(0, handle.tell() - args.tail_bytes))
            handle.readline()
        log = parse(handle)
    finally:
        handle.close()

    print("parsed %d produce/consume events, %d distinct surfaces, "
          "%d downloads, %d edges" %
          (len(log.events), len(log.surfaces), len(log.downloads),
           len(log.edges)))
    sections = args.section.split(",")
    if "overlap" in sections:
        report_overlap(log, args.limit)
    if "stomp" in sections:
        report_stomp(log, args.limit)
    if "orphan" in sections:
        report_orphan(log, args.limit)
    if "predict" in sections:
        report_predict(log, args.limit)
    if "cause" in sections:
        report_cause(log, args.limit)
    if "downloads" in sections:
        report_downloads(log)
    if args.timeline:
        report_timeline(log, int(args.timeline, 0), args.timeline_frames)
    if "edges" in sections:
        report_edges(log, args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
