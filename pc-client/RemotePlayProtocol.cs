using System.Buffers.Binary;

namespace MagicTupperPcClient;

[Flags]
internal enum MtPadButtons : uint
{
    None = 0,
    A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3,
    L = 1u << 4, R = 1u << 5, ZL = 1u << 6, ZR = 1u << 7,
    Minus = 1u << 8, Plus = 1u << 9, LStick = 1u << 10, RStick = 1u << 11,
    Up = 1u << 12, Down = 1u << 13, Left = 1u << 14, Right = 1u << 15,
    Home = 1u << 16, Capture = 1u << 17
}

internal readonly record struct MtGamepadState(
    MtPadButtons Buttons,
    short LeftX, short LeftY, short RightX, short RightY,
    ushort LeftTrigger, ushort RightTrigger)
{
    public static readonly MtGamepadState Neutral = new(MtPadButtons.None, 0, 0, 0, 0, 0, 0);
}

internal readonly record struct MtMotionState(
    float AngleX, float AngleY, float AngleZ,
    float AccelX, float AccelY, float AccelZ,
    bool Valid)
{
    public static readonly MtMotionState None = new(0,0,0,0,0,0,false);
}

internal static class RemotePlayProtocol
{
    public const int InputPort = 8767;
    public const byte Version1 = 1;
    public const byte Version = 2;
    public const byte InputType = 1;
    public const byte AckType = 0x81;
    public const int InputPacketSizeV1 = 40;
    public const int InputPacketSize = 72;
    public const int AckPacketSize = 24;

    // MTRP v2 extends v1 without moving its legacy fields.
    // 36 player, 37 flags(bit0=six-axis valid), 40..63 six float32 values:
    // angle XYZ (radians, integrated from gyro), acceleration XYZ (g).
    public static byte[] Input(uint sequence, ulong timestampUs, MtGamepadState state, byte player = 0, MtMotionState motion = default)
    {
        var p = new byte[InputPacketSize];
        p[0] = (byte)'M'; p[1] = (byte)'T'; p[2] = (byte)'R'; p[3] = (byte)'P';
        p[4] = Version; p[5] = InputType;
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(6, 2), InputPacketSize);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(8, 4), sequence);
        BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(12, 8), timestampUs);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(20, 4), (uint)state.Buttons);
        BinaryPrimitives.WriteInt16LittleEndian(p.AsSpan(24, 2), state.LeftX);
        BinaryPrimitives.WriteInt16LittleEndian(p.AsSpan(26, 2), state.LeftY);
        BinaryPrimitives.WriteInt16LittleEndian(p.AsSpan(28, 2), state.RightX);
        BinaryPrimitives.WriteInt16LittleEndian(p.AsSpan(30, 2), state.RightY);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(32, 2), state.LeftTrigger);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(34, 2), state.RightTrigger);
        p[36] = player < 4 ? player : (byte)0;
        if (motion.Valid)
        {
            p[37] = 1;
            WriteF32(p, 40, motion.AngleX); WriteF32(p, 44, motion.AngleY); WriteF32(p, 48, motion.AngleZ);
            WriteF32(p, 52, motion.AccelX); WriteF32(p, 56, motion.AccelY); WriteF32(p, 60, motion.AccelZ);
        }
        return p;
    }

    private static void WriteF32(byte[] p, int o, float v) => BinaryPrimitives.WriteInt32LittleEndian(p.AsSpan(o,4), BitConverter.SingleToInt32Bits(v));

    public static bool TryParseAck(ReadOnlySpan<byte> p, out uint sequence, out ulong echoedTimestampUs, out ulong switchTimestampUs)
    {
        sequence = 0; echoedTimestampUs = 0; switchTimestampUs = 0;
        if (p.Length != AckPacketSize || p[0] != 'M' || p[1] != 'T' || p[2] != 'R' || p[3] != 'P' ||
            (p[4] != Version && p[4] != Version1) || p[5] != AckType || BinaryPrimitives.ReadUInt16LittleEndian(p.Slice(6, 2)) != AckPacketSize)
            return false;
        sequence = BinaryPrimitives.ReadUInt32LittleEndian(p.Slice(8, 4));
        echoedTimestampUs = BinaryPrimitives.ReadUInt64LittleEndian(p.Slice(12, 8));
        switchTimestampUs = BinaryPrimitives.ReadUInt32LittleEndian(p.Slice(20, 4));
        return true;
    }

    public static ulong MonotonicUs() => (ulong)(System.Diagnostics.Stopwatch.GetTimestamp() * 1_000_000L / System.Diagnostics.Stopwatch.Frequency);
}
