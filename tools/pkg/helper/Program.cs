// SPDX-License-Identifier: AGPL-3.0-or-later
// Separate reader process: original integration code; upstream parsing/codec sources
// remain under their own GPL/AGPL notices. No package/image extraction is performed.
using System.Buffers.Binary;
using System.Text;
using System.IO.Compression;
using System.Text.Json;
using LibProsperoPkg.PKG;
using LibProsperoPkg.PFS;
using LibProsperoPkg.PFS.Compression.Oodle;
using LibProsperoPkg.Util;

internal sealed class TraceCollector
{
    readonly Dictionary<string, (long Offset, byte[] Data)> samples = new();
    public int Bytes { get; private set; }
    public void Add(long offset, byte[] data, int start, int count)
    {
        string name = $"samples/{offset:x16}-{count:x}.bin";
        if (samples.ContainsKey(name) || count > 1024 * 1024 || Bytes + count > 8 * 1024 * 1024) return;
        samples[name] = (offset, data.AsSpan(start, count).ToArray()); Bytes += count;
    }
    public void Save(string output, string package, long size, object layout)
    {
        using var file = new FileStream(output, FileMode.CreateNew, FileAccess.Write);
        using var zip = new ZipArchive(file, ZipArchiveMode.Create);
        foreach (var (name, sample) in samples) { using var stream = zip.CreateEntry(name).Open(); stream.Write(sample.Data); }
        using var report = zip.CreateEntry("report.json").Open();
        JsonSerializer.Serialize(report, new { version = 2, packageName = Path.GetFileName(package), packageSize = size,
            bytesCaptured = Bytes, layout, regions = samples.Select(s => new { file = s.Key, offset = s.Value.Offset, length = s.Value.Data.Length }).ToArray() }, new JsonSerializerOptions { WriteIndented = true });
    }
}

internal sealed class RangeReader : IMemoryReader
{
    readonly FileStream file; readonly long start, length; readonly TraceCollector? trace;
    public RangeReader(FileStream file, long start, long length, TraceCollector? trace = null) { this.file = file; this.start = start; this.length = length; this.trace = trace; }
    public void Read(long pos, byte[] buf, int offset, int count)
    {
        if (pos < 0 || count < 0 || pos > length || count > length - pos) throw new InvalidDataException("Read outside package image");
        file.Position = checked(start + pos); file.ReadExactly(buf.AsSpan(offset, count)); trace?.Add(start + pos, buf, offset, count);
    }
    public void Dispose() { }
}

internal sealed class LazyInner : IMemoryReader
{
    internal record Span(long Logical, int Length, long Stored, int Compressed, int Even, bool Kraken, int Flags = -1);
    readonly IMemoryReader backing; readonly long storedSize;
    readonly List<Span> spans = new();
    readonly Dictionary<int, byte[]> cache = new();
    readonly Queue<int> order = new();
    public long Length { get; }
    public long MetadataHint { get; }
    public LazyInner(IMemoryReader backing, long storedSize, byte[] naps)
    {
        this.backing = backing; this.storedSize = storedSize;
        if (SdkNapsMap.TryRead(naps, storedSize, out var sdkSpans, out long sdkLength, out long sdkMetadata))
        {
            spans.AddRange(sdkSpans); Length = sdkLength; MetadataHint = sdkMetadata; return;
        }
        var doc = ProsperoNapsLayout.Parse(naps);
        if (doc.CblockInfos.Count > 1_000_000) throw new InvalidDataException("NAPS block count exceeds limit");
        long[] raw = doc.FileOffsets.Select(f => checked((long)f.UncompressedOffsetStart)).ToArray();
        var endCandidates = doc.FileOffsets.Where(f => f.Type == 0x40 && f.UncompressedOffsetStart > 0 && (f.UncompressedOffsetStart & 0xffff) == 0).Select(f => (long)f.UncompressedOffsetStart).ToArray();
        Length = endCandidates.Length != 0 ? endCandidates.Max() : raw.Where(v => v > 0 && (v & 0xffff) == 0).DefaultIfEmpty().Max();
        if (Length <= 0 || Length > (1L << 44)) throw new InvalidDataException("Invalid NAPS logical mount size");
        long[] boundaries = raw.Where(v => v > 0 && v <= Length).Distinct().Order().ToArray();
        MetadataHint = boundaries.LastOrDefault(v => v < Length);
        long cursor = 0, logical = 0;
        int boundaryIndex = 0;
        var cb = doc.CblockInfos;
        for (int i = 0; i < cb.Count; ++i)
        {
            var e = cb[i];
            if (e.IsRunBase)
            {
                long fraction = i + 1 < cb.Count && !cb[i + 1].IsRunBase ? cb[i + 1].CoffsetStartMod256K & 0x7fff : 0;
                cursor = checked(((long)e.TweakIdxStart << 15) + fraction); continue;
            }
            if (logical == Length) break;
            if (i + 1 >= cb.Count) throw new InvalidDataException("NAPS missing compressed-length sentinel");
            while (boundaryIndex < boundaries.Length && boundaries[boundaryIndex] <= logical) boundaryIndex++;
            long end = boundaryIndex < boundaries.Length ? boundaries[boundaryIndex] : Length;
            int length = (int)Math.Min(0x40000, end - logical);
            if (length <= 0) throw new InvalidDataException("Invalid NAPS boundary");
            long nextRel = cb[i + 1].IsRunBase ? cb[i + 1].CoffsetEndMod256K : cb[i + 1].CoffsetStartMod256K;
            int compressed = checked((int)(nextRel - e.CoffsetStartMod256K));
            bool kraken = e.KdePredictor == 2;
            int stored = kraken ? compressed : length;
            if (stored <= 0 || stored > 0x100000 || cursor < 0 || cursor > storedSize || stored > storedSize - cursor)
                throw new InvalidDataException($"NAPS stored block exceeds backing file: entry={i}, logical={logical}, storedOffset={cursor}, storedLength={stored}, backingSize={storedSize}, predictor={e.KdePredictor}, currentRelative={e.CoffsetStartMod256K}, nextRelative={nextRel}");
            spans.Add(new Span(logical, length, cursor, compressed, (int)(e.ClenEvenMinus1 / 2 + 1), kraken));
            logical = checked(logical + length); cursor = checked(cursor + stored);
        }
        if (logical != Length) throw new InvalidDataException("NAPS map does not cover the logical mount");
    }
    public object DiagnosticSummary() => new { logicalSize = Length, storedSize, metadataHint = MetadataHint, spanCount = spans.Count,
        tailSpans = spans.Where(s => s.Logical + s.Length > MetadataHint).ToArray() };
    public void CaptureMetadataSamples(TraceCollector trace)
    {
        foreach (var span in spans.Where(s => s.Logical + s.Length > MetadataHint))
        {
            int length = span.Kraken ? span.Compressed : span.Length;
            if (trace.Bytes + length > 8 * 1024 * 1024) break;
            byte[] raw = new byte[length]; backing.Read(span.Stored, raw, 0, raw.Length);
        }
    }
    byte[] Decode(int index)
    {
        if (cache.TryGetValue(index, out var found)) return found;
        var span = spans[index];
        var result = new byte[span.Length];
        if (!span.Kraken) backing.Read(span.Stored, result, 0, result.Length);
        else
        {
            var source = new byte[span.Compressed]; backing.Read(span.Stored, source, 0, source.Length);
            bool ok = false; KrakenDecodeStatus status = KrakenDecodeStatus.Malformed;
            int[] flags = span.Flags >= 0 ? new[] { span.Flags } : span.Length > 0x20000 ? new[] { 0x22, 0x02, 0x12, 0x32, 0x23, 0x03, 0x13, 0x33, 0x00, 0x20 } : new[] { 0x02, 0x00, 0x03, 0x01 };
            foreach (int flag in flags)
            {
                Array.Clear(result);
                try { status = KrakenDecoder.DecodeBlock(source, flag, span.Length > 0x20000 ? span.Even : 0, result); }
                catch (Exception) { continue; }
                if (status == KrakenDecodeStatus.Success) { ok = true; break; }
            }
            if (!ok) throw new InvalidDataException($"Kraken block {index} failed ({status}); this codec form is unsupported");
        }
        // FIFO block cache capped at 64 * 256 KiB = 16 MiB. Reads are serialized by protocol.
        if (cache.Count >= 64) cache.Remove(order.Dequeue());
        cache[index] = result; order.Enqueue(index); return result;
    }
    public void Read(long pos, byte[] buf, int offset, int count)
    {
        if (pos < 0 || count < 0 || pos > Length || count > Length - pos) throw new InvalidDataException("Read outside logical mount");
        while (count != 0)
        {
            int low = 0, high = spans.Count;
            while (low < high) { int mid = low + (high - low) / 2; if (spans[mid].Logical <= pos) low = mid + 1; else high = mid; }
            int index = low - 1;
            if (index < 0) throw new InvalidDataException("Missing logical block");
            var span = spans[index]; int inBlock = checked((int)(pos - span.Logical));
            if (inBlock >= span.Length) throw new InvalidDataException($"Gap in logical block map at offset {pos}; previous block {index} ends at {span.Logical + span.Length}");
            int take = Math.Min(count, span.Length - inBlock);
            Buffer.BlockCopy(Decode(index), inBlock, buf, offset, take);
            pos += take; offset += take; count -= take;
        }
    }
    public void Dispose() { cache.Clear(); }
}

internal sealed class Catalog : IDisposable
{
    internal record Entry(string Name, long Size, bool IsFile, IMemoryReader? Source, long Offset);
    readonly FileStream package; readonly TraceCollector? trace;
    readonly Dictionary<string, Entry> entries = new(StringComparer.Ordinal);
    readonly LazyInner inner = null!;
    public Entry[] Entries { get; }
    public Catalog(string filename, string? diagnosticOutput = null)
    {
        trace = diagnosticOutput is null ? null : new TraceCollector();
        package = new FileStream(filename, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            var header = Read(package, 0, 0x1000);
            if (!header.AsSpan(0, 4).SequenceEqual(new byte[] { 0x7f, 0x46, 0x49, 0x48 }) || header[5] != 0 || U16(header, 6) != 3)
                throw new InvalidDataException("Only debug PS5 FIH format 3 packages are supported");
            long image = checked((long)U64(header, 0x10)), size = checked((long)U64(header, 0x18)), sbAbsolute = checked((long)U64(header, 0x20));
            Bounds(image, size, package.Length);
            if (sbAbsolute < image || sbAbsolute > image + size - 0x400) throw new InvalidDataException("Invalid recorded outer superblock location");
            byte[] sb = Read(package, sbAbsolute, 0x400);
            if (U64(sb, 0) != 2 || U64(sb, 8) != 20130315 || U32(sb, 0x20) != 0x10000 || U64(sb, 0x30) > 10000 || U64(sb, 0x40) > 128)
                throw new InvalidDataException("Unsupported outer superblock geometry");
            bool noAuth = sb.AsSpan(0x370, 16).SequenceEqual(Encoding.ASCII.GetBytes("PPRPLAIN-NOAUTH!"));
            if ((U16(sb, 0x1c) & 4) != 0 && !noAuth)
                throw new InvalidDataException("Encrypted outer PFS is unsupported by this build; the SDK plaintext/no-auth profile is required");
            var imageReader = new RangeReader(package, image, size, trace);
            var outer = new ProsperoPfsReader(imageReader, superblockByteOffset: sbAbsolute - image, skipDecryption: true);
            var nested = outer.GetFile("pfs_image.dat") ?? throw new InvalidDataException("Outer PFS lacks pfs_image.dat");
            var naps = outer.GetFile("naps_pkg_layout.dat") ?? throw new InvalidDataException("Only NAPS data-first inner images are supported");
            if (naps.size < 16 || naps.size > 32 * 1024 * 1024) throw new InvalidDataException("NAPS layout exceeds metadata limit");
            byte[] layout = new byte[(int)naps.size]; naps.GetView().Read(0, layout, 0, layout.Length);
            if (diagnosticOutput is not null)
            {
                // Preserve the already captured outer metadata and NAPS bytes even
                // when this package's block map is not yet understood.
                object diagnostic;
                try
                {
                    inner = new LazyInner(nested.GetView(), nested.size, layout);
                    inner.CaptureMetadataSamples(trace!);
                    diagnostic = new { mappingSucceeded = true, map = inner.DiagnosticSummary() };
                }
                catch (Exception ex) when (ex is InvalidDataException or OverflowException or ArgumentException or EndOfStreamException)
                {
                    diagnostic = new { mappingSucceeded = false, error = ex.Message,
                        storedSize = nested.size, napsSize = layout.Length };
                    Console.Error.WriteLine("Mapping unresolved; saving raw layout diagnostic: " + ex.Message);
                }
                trace!.Save(diagnosticOutput, filename, package.Length, diagnostic);
                Entries = Array.Empty<Entry>(); return;
            }
            inner = new LazyInner(nested.GetView(), nested.size, layout);
            Add(new Entry("", 0, false, null, 0));
            ReadTree();
            OverlayCnt(header);
            Entries = entries.Values.OrderBy(e => e.Name, StringComparer.Ordinal).ToArray();
            if (!entries.TryGetValue("eboot.bin", out var eboot) || !eboot.IsFile) throw new InvalidDataException("Mounted application lacks eboot.bin");
        }
        catch { package.Dispose(); throw; }
    }
    static void Bounds(long offset, long size, long length)
    {
        if (offset < 0 || size < 0 || offset > length || size > length - offset) throw new InvalidDataException("Package range is invalid");
    }
    byte[] Read(FileStream file, long offset, int length)
    {
        Bounds(offset, length, file.Length); byte[] bytes = new byte[length]; file.Position = offset; file.ReadExactly(bytes); trace?.Add(offset, bytes, 0, length); return bytes;
    }
    static ulong U64(byte[] b, int p) => BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan(p));
    static uint U32(byte[] b, int p) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p));
    static ushort U16(byte[] b, int p) => BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(p));
    byte[] InnerRead(long offset, int count) { byte[] b = new byte[count]; inner.Read(offset, b, 0, count); return b; }
    static void SafeName(string name)
    {
        if (name.Length == 0 || name is "." or ".." || name.Contains('/') || name.Contains('\\') || name.Contains(':') || name.Contains('\0'))
            throw new InvalidDataException("Unsafe package member name");
    }
    void Add(Entry entry)
    {
        if (entry.Name.Length > 4096 || entries.Count > 200000) throw new InvalidDataException("Package catalog limit exceeded");
        if (entries.TryGetValue(entry.Name, out var old) && old.IsFile != entry.IsFile) throw new InvalidDataException("File/directory path collision");
        entries[entry.Name] = entry;
        int split = entry.Name.LastIndexOf('/');
        if (split > 0) Add(new Entry(entry.Name[..split], 0, false, null, 0));
    }
    record Node(ushort Mode, long Size, long Offset);
    void ReadTree()
    {
        long sbOffset = -1;
        if (inner.MetadataHint >= 0 && inner.MetadataHint <= inner.Length - 16)
        {
            byte[] hint = InnerRead(inner.MetadataHint, 16);
            if (U64(hint, 0) == 2 && U64(hint, 8) == 20130315) sbOffset = inner.MetadataHint;
        }
        long floor = Math.Max(0, inner.Length - 4 * 1024 * 1024);
        for (long p = (inner.Length - 0x10000) & ~0xffffL; sbOffset < 0 && p >= floor; p -= 0x10000)
        {
            byte[] probe = InnerRead(p, 16);
            if (U64(probe, 0) == 2 && U64(probe, 8) == 20130315) { sbOffset = p; break; }
        }
        if (sbOffset < 0) throw new InvalidDataException("Inner superblock not found in final 4 MiB; layout unsupported");
        byte[] sb = InnerRead(sbOffset, 0x80);
        int bs = checked((int)U32(sb, 0x20)), count = checked((int)U64(sb, 0x30));
        if (bs != 0x10000 || count <= 0 || count > 200000) throw new InvalidDataException($"Unsupported inner inode geometry: blockSize={bs}, inodeCount={count}");
        long inodeStart = checked(sbOffset + bs);
        int inodesPerBlock = bs / 0xa8;
        byte[] table = InnerRead(inodeStart, checked(((count + inodesPerBlock - 1) / inodesPerBlock) * bs));
        var nodes = new Node[count];
        for (int i = 0; i < count; ++i)
        {
            int at = (i / inodesPerBlock) * bs + (i % inodesPerBlock) * 0xa8; long fileSize = checked((long)U64(table, at + 8)), offset = checked((long)U64(table, at + 0x60));
            if (offset < 0 || fileSize < 0 || offset > inner.Length || fileSize > inner.Length - offset) throw new InvalidDataException($"Inner inode {i}: offset={offset} size={fileSize} exceeds image {inner.Length}");
            nodes[i] = new Node(U16(table, at), fileSize, offset);
        }
        var seen = new HashSet<uint>();
        var stack = new Stack<(uint Id, string Path, bool Under, int Depth)>(); stack.Push((0, "", false, 0));
        while (stack.TryPop(out var item))
        {
            if (item.Id >= nodes.Length || item.Depth > 128 || !seen.Add(item.Id)) throw new InvalidDataException("Invalid/cyclic inner directory tree");
            var node = nodes[item.Id];
            if ((node.Mode & 0xf000) != 0x4000 || node.Size > 32 * 1024 * 1024) throw new InvalidDataException("Invalid/oversized directory inode");
            byte[] data = InnerRead(node.Offset, (int)node.Size);
            for (int pos = 0; pos <= data.Length - 16;)
            {
                uint id = U32(data, pos), type = U32(data, pos + 4), nameLength = U32(data, pos + 8), entrySize = U32(data, pos + 12);
                if (entrySize == 0) break;
                if (entrySize < 16 || entrySize > data.Length - pos || nameLength > entrySize - 16) throw new InvalidDataException("Invalid inner dirent");
                string name = Encoding.UTF8.GetString(data, pos + 16, (int)nameLength).TrimEnd('\0'); pos += (int)entrySize;
                if (type is 4 or 5 || name is "." or "..") continue;
                if (type is not (2 or 3) || id >= nodes.Length) throw new InvalidDataException("Unsupported inner dirent type/index");
                SafeName(name);
                bool under = item.Under || (item.Id == 0 && name == "uroot");
                string path = item.Path.Length == 0 ? (under && name == "uroot" ? "" : name) : item.Path + "/" + name;
                if (type == 3)
                {
                    if (under) Add(new Entry(path, 0, false, null, 0));
                    stack.Push((id, path, under, item.Depth + 1));
                }
                else if (item.Under)
                {
                    if ((nodes[id].Mode & 0xf000) != 0x8000) throw new InvalidDataException("File inode mode mismatch");
                    Add(new Entry(path, nodes[id].Size, true, inner, nodes[id].Offset));
                }
            }
        }
    }
    void OverlayCnt(byte[] fih)
    {
        long offset = checked((long)U64(fih, 0x58));
        if (offset <= 0) throw new InvalidDataException("Embedded CNT is required for package metadata");
        byte[] hdr = Read(package, offset, 0x1000);
        if (!hdr.AsSpan(0, 4).SequenceEqual(new byte[] { 0x7f, 0x43, 0x4e, 0x54 })) throw new InvalidDataException("Invalid CNT magic");
        uint count = BinaryPrimitives.ReadUInt32BigEndian(hdr.AsSpan(0x10));
        uint tableOffset = BinaryPrimitives.ReadUInt32BigEndian(hdr.AsSpan(0x18));
        if (count > 65536 || tableOffset < 0x1000) throw new InvalidDataException("Invalid CNT table range");
        byte[] table = Read(package, checked(offset + tableOffset), checked((int)count * 32));
        var records = new List<(uint Id, uint Name, uint Flags, uint Offset, uint Size)>();
        for (int i = 0; i < count; ++i)
        {
            var r = table.AsSpan(i * 32, 32);
            records.Add((BinaryPrimitives.ReadUInt32BigEndian(r), BinaryPrimitives.ReadUInt32BigEndian(r[4..]),
                BinaryPrimitives.ReadUInt32BigEndian(r[8..]), BinaryPrimitives.ReadUInt32BigEndian(r[16..]), BinaryPrimitives.ReadUInt32BigEndian(r[20..])));
        }
        var namesEntry = records.FirstOrDefault(r => r.Id == 0x200);
        if (namesEntry.Size > 1024 * 1024 || (namesEntry.Flags & 0x80000000) != 0) throw new InvalidDataException("Unsupported CNT name table");
        byte[] names = Read(package, checked(offset + namesEntry.Offset), (int)namesEntry.Size);
        foreach (var r in records)
        {
            if (r.Id < 0x1000 || (r.Flags & 0x80000000) != 0) continue; // encrypted optional metadata is not exposed as raw ciphertext
            string? name = r.Id == 0x2000 ? "param.json" : null;
            if (name is null && r.Name > 0 && r.Name < names.Length)
            {
                int start = (int)r.Name, end = Array.IndexOf(names, (byte)0, start);
                if (end < start || end - start > 4096) throw new InvalidDataException("Malformed CNT entry name");
                name = Encoding.UTF8.GetString(names, start, end - start);
            }
            if (name is null) continue;
            foreach (string part in name.Split('/')) SafeName(part);
            Bounds(checked(offset + r.Offset), r.Size, package.Length);
            Add(new Entry("sce_sys/" + name, r.Size, true, new RangeReader(package, checked(offset + r.Offset), r.Size), 0));
        }
    }
    public byte[] ReadFile(int id, long offset, int length)
    {
        if (id < 0 || id >= Entries.Length || !Entries[id].IsFile || length < 0 || length > 1024 * 1024 || offset < 0) throw new InvalidDataException("Invalid file read request");
        var e = Entries[id];
        if (offset >= e.Size) return Array.Empty<byte>();
        int size = (int)Math.Min(length, e.Size - offset); byte[] result = new byte[size];
        e.Source!.Read(checked(e.Offset + offset), result, 0, size); return result;
    }
    public void Dispose() { inner?.Dispose(); package.Dispose(); }
}

internal static class Program
{
    static void String(BinaryWriter w, string value) { byte[] bytes = Encoding.UTF8.GetBytes(value); w.Write((uint)bytes.Length); w.Write(bytes); }
    static int Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "--check")
        {
            try
            {
                using var c = new Catalog(args[1]);
                int id = Array.FindIndex(c.Entries, e => e.Name == "eboot.bin");
                byte[] header = c.ReadFile(id, 0, 64);
                int paramId = Array.FindIndex(c.Entries, e => e.Name == "sce_sys/param.json");
                if (paramId >= 0) c.ReadFile(paramId, 0, 64);
                Console.WriteLine(JsonSerializer.Serialize(new { catalogOpened = true,
                    entries = c.Entries.Length, files = c.Entries.Count(e => e.IsFile),
                    ebootSize = c.Entries[id].Size, ebootHeaderHex = Convert.ToHexString(header),
                    note = "Catalog and initial reads passed; game boot has not been tested." }, new JsonSerializerOptions { WriteIndented = true }));
                return 0;
            }
            catch (Exception ex) { Console.Error.WriteLine("Mount check failed: " + ex.Message); return 2; }
        }
        if (args.Length == 3 && args[0] == "--diagnose")
        {
            try { using var c = new Catalog(args[1], args[2]); Console.WriteLine("Created " + Path.GetFullPath(args[2])); return 0; }
            catch (Exception ex) { Console.Error.WriteLine("Diagnostic failed: " + ex.Message); return 2; }
        }
        using var writer = new BinaryWriter(Console.OpenStandardOutput(), Encoding.UTF8, leaveOpen: true);
        try
        {
            if (args.Length != 1) throw new ArgumentException("Pass the package filename");
            using var catalog = new Catalog(args[0]);
            writer.Write(Encoding.ASCII.GetBytes("KPK1")); writer.Write((uint)catalog.Entries.Length);
            foreach (var e in catalog.Entries) { String(writer, e.Name); writer.Write((ulong)e.Size); writer.Write((byte)(e.IsFile ? 1 : 0)); }
            writer.Flush();
            using var reader = new BinaryReader(Console.OpenStandardInput(), Encoding.UTF8);
            for (;;)
            {
                int op;
                try { op = reader.ReadByte(); } catch (EndOfStreamException) { return 0; }
                if (op != 1) throw new InvalidDataException("Unknown package-reader protocol operation");
                ulong id = reader.ReadUInt64(), offset = reader.ReadUInt64(); uint size = reader.ReadUInt32();
                try
                {
                    byte[] data = catalog.ReadFile(checked((int)id), checked((long)offset), checked((int)size));
                    writer.Write((uint)data.Length); writer.Write(data); writer.Flush();
                }
                catch (Exception ex) { writer.Write(uint.MaxValue); String(writer, ex.Message); writer.Flush(); }
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("PKG mount: " + ex.ToString());
            writer.Write(Encoding.ASCII.GetBytes("KPE1")); String(writer, ex.Message); writer.Flush(); return 2;
        }
    }
}
