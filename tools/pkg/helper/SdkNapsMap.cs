// SPDX-License-Identifier: AGPL-3.0-or-later
using System.Buffers.Binary;
using LibProsperoPkg.PKG;

internal static class SdkNapsMap
{
    public static bool TryRead(byte[] bytes, long backingSize, out List<LazyInner.Span> spans, out long length, out long metadata)
    {
        spans = new(); length = metadata = 0;
        var counts = ProsperoNapsLayout.DecodeHeader(bytes);
        int fstart = checked(16 + counts.NumOuterBlocks * 8 + counts.NumShufflePatterns * 8);
        int fend = checked(fstart + counts.NumFiles * 6);
        if (counts.NumFiles < 1 || fend > bytes.Length || bytes[fend - 1] != 0x40) return false;
        // SDK layout: NumFiles includes the end marker; CblockInfo is aligned to 8 bytes.
        int ustart = fend;
        int cstart = checked((ustart + counts.NumU2cEntries * 10 + 7) & ~7);
        int cend = checked(cstart + counts.NumCblockInfo * 9);
        if (cend > bytes.Length || bytes.Length - cend > 16) throw new InvalidDataException("Unsupported SDK NAPS section bounds");
        if (counts.NumUBlocks < 1 || counts.NumUBlocks > 1_000_000 || counts.NumCblockInfo > 1_000_000)
            throw new InvalidDataException("SDK NAPS count limit exceeded");
        var boundaries = new List<long>();
        for (int i = 0; i < counts.NumFiles; i++)
        {
            var f = ProsperoNapsLayout.DecodeFileOffsetEntry(bytes.AsSpan(fstart + 6 * i, 6));
            long offset = checked((long)f.UncompressedOffsetStart);
            if (boundaries.Count > 0 && offset < boundaries[^1]) throw new InvalidDataException("Unordered SDK file offsets");
            if (boundaries.Count == 0 || boundaries[^1] != offset) boundaries.Add(offset);
        }
        length = boundaries[^1]; metadata = boundaries.Count > 1 ? boundaries[^2] : 0;
        if (length <= 0 || length > (1L << 44)) throw new InvalidDataException("Invalid SDK image length");
        var first = new int[counts.NumUBlocks];
        for (int u = 0; u < first.Length; u++)
        {
            int at = ustart + (u / 8) * 10;
            int index = bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16;
            if (u % 8 != 0) index += bytes[at + 2 + u % 8];
            if (index >= counts.NumCblockInfo || (u > 0 && index < first[u - 1])) throw new InvalidDataException("Invalid SDK block index");
            first[u] = index;
        }
        var records = new List<NapsCblockInfoEntry>();
        for (int i = 0; i < counts.NumCblockInfo; i++) records.Add(ProsperoNapsLayout.DecodeCblockInfoEntry(bytes.AsSpan(cstart + 9 * i, 9)));
        long cursor = 0, previousEnd = 0; int group = 0, boundary = 0;
        for (int i = 0; i + 1 < records.Count; i++)
        {
            var e = records[i]; var next = records[i + 1];
            if (e.IsRunBase)
            {
                cursor = checked(((long)e.CoffsetStart256K / 2) * 0x40000 + (next.IsRunBase ? 0 : next.CoffsetStartMod256K));
                continue;
            }
            while (group + 1 < first.Length && first[group + 1] <= i) group++;
            if (first[group] > i) throw new InvalidDataException("SDK block precedes its index group");
            // SDK logical offset uses bits 20..37; bit 37 is exposed by
            // the upstream model as the low bit of ClenEvenMinus1.
            long relative = e.UoffsetStart / 2 + ((long)(e.ClenEvenMinus1 & 1) << 17);
            long logical = (long)group * 0x40000 + relative;
            if (logical < previousEnd) throw new InvalidDataException("Overlapping SDK logical blocks");
            if (logical >= ((long)group + 1) * 0x40000 || logical >= length) throw new InvalidDataException("SDK logical block outside group");
            while (boundary < boundaries.Count && boundaries[boundary] <= logical) boundary++;
            if (boundary == boundaries.Count) throw new InvalidDataException("Missing SDK boundary");
            // SDK chunk-length-minus-one occupies bits 38..54. The upstream
            // model exposes bit 54 separately as Even; retain this high bit
            // for first chunks whose stored length exceeds 64 KiB.
            int even = checked((int)e.ClenEvenMinus1 / 2 + (e.Even << 16) + 1);
            int blockLength = (int)Math.Min(0x40000, boundaries[boundary] - logical);
            bool compressed;
            // Mode 7 combines sub-literals, LZ, and the first-chunk restart
            // flag. DecodeBlock already restarts the first chunk implicitly.
            if (e.KdePredictor is not (0 or 2 or 3 or 4 or 6 or 7)) throw new InvalidDataException($"Unsupported SDK codec mode {e.KdePredictor} at record {i}");
            long nextRelative = next.IsRunBase ? next.CoffsetEndMod256K : next.CoffsetStartMod256K;
            int stored = (int)((nextRelative - e.CoffsetStartMod256K) & 0x3ffff);
            if (stored == 0 && blockLength == 0x40000) stored = blockLength;
            compressed = stored != blockLength;
            if (blockLength <= 0 || logical + blockLength > length || stored <= 0 || cursor < 0 || cursor > backingSize - stored)
                throw new InvalidDataException($"SDK block {i} exceeds image bounds");
            int flags = e.KdePredictor | (e.ShuffleIdx << 4);
            spans.Add(new LazyInner.Span(logical, blockLength, cursor, stored, even, compressed, flags));
            previousEnd = logical + blockLength; cursor += stored;
        }
        if (previousEnd != length) throw new InvalidDataException("SDK map does not reach image end");
        return true;
    }
}
