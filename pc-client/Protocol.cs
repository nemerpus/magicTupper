using System.Buffers.Binary;
using System.Text;

namespace MagicTupperPcClient;

internal enum Command : byte
{
    Hello = 1, List = 2, Stat = 3, Read = 4, Write = 5, MakeDirectory = 6, Remove = 7, Lock = 8, Unmount = 9,
    BatchBegin = 10, BatchFile = 11, BatchSelect = 12, BatchAction = 13, BatchDestination = 14, BatchCommit = 15, BatchStatus = 16, BatchResume = 17,
    TransferBegin = 0x20, TransferData = 0x21, TransferEnd = 0x22, TransferCancel = 0x23, TransferStatus = 0x24, DirectStart = 0x25, DirectPoll = 0x26, DirectData = 0x27, LinkBenchStart = 0x28, LinkBenchData = 0x29, LinkBenchEnd = 0x2A, NetHttpBench = 0x2B
}

internal static class Protocol
{
    public const string Magic = "MTUSB1";
    public const int MaxFrame = 2 * 1024 * 1024;
    public const int HeaderSize = 12;
    public const int MaxPayload = MaxFrame - HeaderSize;
    public const int TransferDataChunk = MaxFrame - HeaderSize - 16 - 32; // margen de 32 bytes; frame de ~2 MiB para USB y TCP
    public const int DirectDataChunk = MaxPayload - 16 - 32; // fallback MTUSB1
    public const int DirectRequestMax = 32 * 1024 * 1024; // MT2.4: doble buffer 2x32 MiB = 64 MiB efectivos

    public static byte[] Frame(Command command, ReadOnlySpan<byte> payload)
    {
        if (payload.Length > MaxPayload) throw new ArgumentOutOfRangeException(nameof(payload));
        var frame = new byte[HeaderSize + payload.Length];
        Encoding.ASCII.GetBytes(Magic).CopyTo(frame, 0);
        frame[6] = 1;
        frame[7] = (byte)command;
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8), (uint)payload.Length);
        payload.CopyTo(frame.AsSpan(12));
        return frame;
    }

    public static bool TryReadResponse(ReadOnlySpan<byte> frame, Command expectedCommand, out byte[] payload, out string error)
    {
        payload = Array.Empty<byte>();
        error = string.Empty;
        if (frame.Length < HeaderSize)
        {
            error = "respuesta incompleta";
            return false;
        }
        if (!frame[..6].SequenceEqual(Encoding.ASCII.GetBytes(Magic)))
        {
            error = "magic MTUSB1 no válido";
            return false;
        }
        if (frame[6] != 1)
        {
            error = $"versión USB no compatible: {frame[6]}";
            return false;
        }
        var responseCommand = frame[7];
        if (responseCommand != ((byte)expectedCommand | 0x80))
        {
            error = $"comando de respuesta inesperado: 0x{responseCommand:X2}";
            return false;
        }
        var length = BinaryPrimitives.ReadUInt32LittleEndian(frame[8..12]);
        if (length > MaxPayload || frame.Length != HeaderSize + length)
        {
            error = $"longitud de respuesta no válida: {length}";
            return false;
        }
        payload = frame[HeaderSize..].ToArray();
        return true;
    }

    public static byte[] Path(string relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath) || relativePath.Contains('\\') || relativePath.StartsWith('/') || relativePath.Split('/').Any(p => p is "" or "." or ".."))
            throw new ArgumentException("Ruta relativa no válida.", nameof(relativePath));
        return Encoding.UTF8.GetBytes(relativePath);
    }

    public static byte[] SdPath(string relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath) || relativePath.Trim() == "/") return Array.Empty<byte>();
        relativePath = relativePath.Replace('\\', '/').Trim('/');
        if (relativePath.Split('/').Any(p => p is "" or "." or "..")) throw new ArgumentException("Ruta SD no válida.", nameof(relativePath));
        return Encoding.UTF8.GetBytes("sdmc:/" + relativePath);
    }

    public static byte[] ReadRequest(string relativePath, ulong offset, uint length)
    {
        var path = SdPath(relativePath);
        if (path.Length == 0 || path.Length > 4096 || length == 0 || length > 512 * 1024) throw new ArgumentException("Solicitud READ no válida.", nameof(relativePath));
        var payload = new byte[16 + path.Length]; payload[0] = (byte)'R'; payload[1] = (byte)'2';
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(2), offset); BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(10), length); BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(14), (ushort)path.Length); path.CopyTo(payload.AsSpan(16)); return payload;
    }

    public static byte[] WriteRequest(string relativePath, ulong offset, ReadOnlySpan<byte> content)
    {
        var path = SdPath(relativePath); if (path.Length == 0 || path.Length > 4096 || content.Length > 1024 * 1024) throw new ArgumentException("Solicitud WRITE no válida.", nameof(relativePath));
        var payload = new byte[16 + path.Length + content.Length]; payload[0] = (byte)'W'; payload[1] = (byte)'2'; BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(2), offset); BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(10), (uint)content.Length); BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(14), (ushort)path.Length); path.CopyTo(payload.AsSpan(16)); content.CopyTo(payload.AsSpan(16 + path.Length)); return payload;
    }

    // ---------------------------------------------------------------------
    // Comandos de lote (batch) reales que espera la Switch.
    // ---------------------------------------------------------------------

    // BATCH_BEGIN (10): payload = 'B','2' + count u32 LE
    public static byte[] BatchBegin(int count)
    {
        var p = new byte[6];
        p[0] = (byte)'B'; p[1] = (byte)'2';
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(2), (uint)count);
        return Frame(Command.BatchBegin, p);
    }

    // BATCH_FILE (11): payload = 'B','2' + index u8 + size u64 LE + nameLen u16 LE + name
    public static byte[] BatchFile(byte index, string name, long size)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        if (nameBytes.Length == 0 || nameBytes.Length > 4096) throw new ArgumentException("Nombre no válido", nameof(name));
        var p = new byte[13 + nameBytes.Length];
        p[0] = (byte)'B'; p[1] = (byte)'2'; p[2] = index;
        BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(3), (ulong)size);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(11), (ushort)nameBytes.Length);
        nameBytes.CopyTo(p.AsSpan(13));
        return Frame(Command.BatchFile, p);
    }

    // BATCH_SELECT (12): payload = index u8 + selected u8
    public static byte[] BatchSelect(byte index, bool selected)
    {
        var p = new byte[2];
        p[0] = index; p[1] = (byte)(selected ? 1 : 0);
        return Frame(Command.BatchSelect, p);
    }

    // BATCH_ACTION (13): payload = action u8 + destination u8
    public static byte[] BatchAction(byte action, byte destination)
    {
        var p = new byte[2];
        p[0] = action; p[1] = destination;
        return Frame(Command.BatchAction, p);
    }

    // BATCH_COMMIT (15): payload = index u8
    public static byte[] BatchCommit(byte index)
    {
        var p = new byte[1];
        p[0] = index;
        return Frame(Command.BatchCommit, p);
    }

    // BATCH_WRITE (comando 5, subformato 'B','W'): escribe al temporal de la Switch
    // payload = 'B','W' + index u8 + offset u64 LE + amount u32 LE + content
    public static byte[] BatchWrite(byte index, ulong offset, ReadOnlySpan<byte> content)
    {
        if (offset < 0) throw new ArgumentOutOfRangeException(nameof(offset));
        var p = new byte[15 + content.Length];
        p[0] = (byte)'B'; p[1] = (byte)'W'; p[2] = index;
        BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(3), offset);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(11), (uint)content.Length);
        content.CopyTo(p.AsSpan(15));
        return Frame(Command.Write, p);
    }

    public static byte[] BatchStatus() => Frame(Command.BatchStatus, ReadOnlySpan<byte>.Empty);


    public static byte[] TransferBegin(string name, long size, uint fileIndex, uint totalFiles)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        if (nameBytes.Length == 0 || nameBytes.Length > 4096) throw new ArgumentException("Nombre no válido", nameof(name));
        if (size < 0 || totalFiles == 0 || fileIndex >= totalFiles) throw new ArgumentOutOfRangeException(nameof(fileIndex));
        var p = new byte[18 + nameBytes.Length];
        BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(0), (ulong)size);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(8), fileIndex);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(12), totalFiles);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(16), (ushort)nameBytes.Length);
        nameBytes.CopyTo(p.AsSpan(18));
        return Frame(Command.TransferBegin, p);
    }

    public static bool TryParseTransferDecision(ReadOnlySpan<byte> payload, out byte action, out string path, out string error)
    {
        action = 0; path = string.Empty; error = string.Empty;
        if (payload.Length < 3) { error = "respuesta TRANSFER_BEGIN incompleta"; return false; }
        action = payload[0];
        var pathLen = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(1, 2));
        if (3 + pathLen != payload.Length) { error = "longitud de ruta no válida"; return false; }
        path = pathLen == 0 ? string.Empty : Encoding.UTF8.GetString(payload.Slice(3, pathLen));
        if (action > 3) { error = $"acción no válida: {action}"; return false; }
        return true;
    }

    public static byte[] TransferData(uint fileIndex, ulong offset, ReadOnlySpan<byte> content)
    {
        if (content.Length <= 0 || content.Length > TransferDataChunk) throw new ArgumentOutOfRangeException(nameof(content));
        var p = new byte[16 + content.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(0), fileIndex);
        BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(4), offset);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(12), (uint)content.Length);
        content.CopyTo(p.AsSpan(16));
        return Frame(Command.TransferData, p);
    }

    public static bool TryParseTransferAck(ReadOnlySpan<byte> payload, out ulong received)
    {
        received = 0;
        if (payload.Length != 8) return false;
        received = BinaryPrimitives.ReadUInt64LittleEndian(payload);
        return true;
    }

    public static byte[] TransferEnd(uint fileIndex)
    {
        var p = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(p, fileIndex);
        return Frame(Command.TransferEnd, p);
    }

    public static byte[] TransferCancel() => Frame(Command.TransferCancel, ReadOnlySpan<byte>.Empty);
    public static byte[] TransferStatus() => Frame(Command.TransferStatus, ReadOnlySpan<byte>.Empty);
    public static byte[] DirectStart(uint index)
    {
        var p = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(p, index); return Frame(Command.DirectStart, p);
    }
    public static byte[] DirectPoll() => Frame(Command.DirectPoll, ReadOnlySpan<byte>.Empty);
    public static bool TryParseDirectRequest(ReadOnlySpan<byte> payload, out uint requestId, out ulong offset, out uint length)
    {
        requestId=0; offset=0; length=0;
        if(payload.Length!=18 || payload[0]!=(byte)'R' || payload[1]!=(byte)'Q') return false;
        requestId=BinaryPrimitives.ReadUInt32LittleEndian(payload[2..6]); offset=BinaryPrimitives.ReadUInt64LittleEndian(payload[6..14]); length=BinaryPrimitives.ReadUInt32LittleEndian(payload[14..18]);
        return length>0 && length<=DirectRequestMax;
    }
    public static byte[] LinkBenchStart(ulong totalBytes) { var p=new byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(p,totalBytes); return Frame(Command.LinkBenchStart,p); }
    public static byte[] LinkBenchData(uint sequence, ReadOnlySpan<byte> data) { if(data.Length<=0||data.Length>TransferDataChunk) throw new ArgumentOutOfRangeException(nameof(data)); var p=new byte[4+data.Length]; BinaryPrimitives.WriteUInt32LittleEndian(p,sequence); data.CopyTo(p.AsSpan(4)); return Frame(Command.LinkBenchData,p); }
    public static byte[] LinkBenchEnd() => Frame(Command.LinkBenchEnd, ReadOnlySpan<byte>.Empty);

    public static byte[] NetHttpBench(string url, ulong totalBytes, byte streams)
    {
        if (streams < 1 || streams > 4) throw new ArgumentOutOfRangeException(nameof(streams));
        var u = Encoding.UTF8.GetBytes(url);
        if (u.Length == 0 || u.Length > 1024) throw new ArgumentException("URL benchmark no válida", nameof(url));
        var p = new byte[11 + u.Length];
        p[0] = streams;
        BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(1, 8), totalBytes);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(9, 2), (ushort)u.Length);
        u.CopyTo(p.AsSpan(11));
        return Frame(Command.NetHttpBench, p);
    }

    public static byte[] DirectData(uint requestId, ulong offset, ReadOnlySpan<byte> data)
    {
        if(data.Length==0 || data.Length>DirectDataChunk) throw new ArgumentOutOfRangeException(nameof(data));
        var p=new byte[16+data.Length]; BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(0,4),requestId); BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(4,8),offset); BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(12,4),(uint)data.Length); data.CopyTo(p.AsSpan(16)); return Frame(Command.DirectData,p);
    }
}
internal enum Mtp2Type : byte
{
    Hello = 0x01,
    BenchmarkOpen = 0x10,
    BenchmarkData = 0x11,
    BenchmarkClose = 0x13,
    TransferData = 0x21,
    DirectData = 0x27,
    AckMask = 0x80
}

[Flags]
internal enum Mtp2Flags : ushort
{
    None = 0,
    AckRequired = 1,
    EndOfStream = 2,
    Error = 4
}

internal readonly record struct Mtp2Header(Mtp2Type Type, Mtp2Flags Flags, uint StreamId, uint Sequence, uint PayloadLength, uint AckSequence, ulong Offset);

// MT2 is the high-throughput framed data plane.  MTUSB1 remains the control plane
// so old clients/NROs can still fall back cleanly.
internal static class Mtp2
{
    public const string Magic = "MTP2";
    public const byte Version = 2;
    public const int HeaderSize = 32;
    public const int ChunkSize = 1024 * 1024;
    public const int UsbWindowSize = 128 * 1024 * 1024; // MT2.4: benchmark perfila 8/16/32/64/128 MiB
    public const int NetworkChunkSize = 512 * 1024;
    public const int NetworkWindowSize = 8 * 1024 * 1024; // techo; autoajuste 1..8 MiB
    public const int WindowSize = UsbWindowSize; // máximo negociable MT2.4
    public const int MaxFrame = HeaderSize + ChunkSize;

    public static byte[] Frame(Mtp2Type type, Mtp2Flags flags, uint streamId, uint sequence, ulong offset, ReadOnlySpan<byte> payload, uint ackSequence = 0)
    {
        if (payload.Length > ChunkSize) throw new ArgumentOutOfRangeException(nameof(payload));
        var frame = new byte[HeaderSize + payload.Length];
        WriteHeader(frame, type, flags, streamId, sequence, offset, payload.Length, ackSequence);
        payload.CopyTo(frame.AsSpan(HeaderSize));
        return frame;
    }

    public static void WriteHeader(byte[] frame, Mtp2Type type, Mtp2Flags flags, uint streamId, uint sequence, ulong offset, int payloadLength, uint ackSequence = 0)
    {
        if (payloadLength < 0 || payloadLength > ChunkSize || frame.Length != HeaderSize + payloadLength)
            throw new ArgumentOutOfRangeException(nameof(payloadLength));
        frame[0] = (byte)'M'; frame[1] = (byte)'T'; frame[2] = (byte)'P'; frame[3] = (byte)'2';
        frame[4] = Version;
        frame[5] = (byte)type;
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6, 2), (ushort)flags);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8, 4), streamId);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(12, 4), sequence);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(16, 4), (uint)payloadLength);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(20, 4), ackSequence);
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(24, 8), offset);
    }

    public static byte[] Hello(uint desiredChunk = ChunkSize, uint desiredWindow = UsbWindowSize)
    {
        var payload = new byte[12];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), desiredChunk);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), desiredWindow);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), 1); // feature bit0: cumulative/window ACK
        return Frame(Mtp2Type.Hello, Mtp2Flags.AckRequired, 0, 0, 0, payload);
    }

    public static bool TryParse(ReadOnlySpan<byte> frame, out Mtp2Header header, out ReadOnlySpan<byte> payload, out string error)
    {
        header = default; payload = default; error = string.Empty;
        if (frame.Length < HeaderSize) { error = "MT2 header incompleto"; return false; }
        if (!frame[..4].SequenceEqual(Encoding.ASCII.GetBytes(Magic))) { error = "magic MT2 inválido"; return false; }
        if (frame[4] != Version) { error = $"versión MT2 no compatible: {frame[4]}"; return false; }
        var length = BinaryPrimitives.ReadUInt32LittleEndian(frame.Slice(16, 4));
        if (length > ChunkSize || frame.Length != HeaderSize + length) { error = $"longitud MT2 inválida: {length}"; return false; }
        header = new Mtp2Header((Mtp2Type)frame[5], (Mtp2Flags)BinaryPrimitives.ReadUInt16LittleEndian(frame.Slice(6, 2)), BinaryPrimitives.ReadUInt32LittleEndian(frame.Slice(8, 4)), BinaryPrimitives.ReadUInt32LittleEndian(frame.Slice(12, 4)), length, BinaryPrimitives.ReadUInt32LittleEndian(frame.Slice(20, 4)), BinaryPrimitives.ReadUInt64LittleEndian(frame.Slice(24, 8)));
        payload = frame.Slice(HeaderSize, (int)length);
        return true;
    }
}
