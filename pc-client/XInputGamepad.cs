using System.Runtime.InteropServices;

namespace MagicTupperPcClient;

internal sealed class XInputGamepad
{
    [StructLayout(LayoutKind.Sequential)] private struct XINPUT_GAMEPAD
    {
        public ushort wButtons; public byte bLeftTrigger; public byte bRightTrigger;
        public short sThumbLX; public short sThumbLY; public short sThumbRX; public short sThumbRY;
    }
    [StructLayout(LayoutKind.Sequential)] private struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate uint XInputGetStateDelegate(uint index, out XINPUT_STATE state);
    [StructLayout(LayoutKind.Sequential)] private struct XINPUT_VIBRATION { public ushort wLeftMotorSpeed; public ushort wRightMotorSpeed; }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate uint XInputSetStateDelegate(uint index, ref XINPUT_VIBRATION vibration);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern nint GetProcAddress(nint hModule, nint ordinal);

    private readonly nint library;
    private readonly XInputGetStateDelegate? getState;
    private readonly XInputGetStateDelegate? getStateEx;
    private readonly XInputSetStateDelegate? setState;

    public XInputGamepad()
    {
        foreach (var dll in new[] { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
        {
            if (!NativeLibrary.TryLoad(dll, out var loaded)) continue;
            if (NativeLibrary.TryGetExport(loaded, "XInputGetState", out var proc))
            {
                library = loaded;
                getState = Marshal.GetDelegateForFunctionPointer<XInputGetStateDelegate>(proc);
                if (NativeLibrary.TryGetExport(loaded, "XInputSetState", out var setProc)) setState = Marshal.GetDelegateForFunctionPointer<XInputSetStateDelegate>(setProc);

                // Microsoft XInput hides the Guide/Xbox button from XInputGetState. On
                // xinput1_4/1_3 Windows exposes XInputGetStateEx as ordinal 100. Use it
                // opportunistically and fall back to the documented API if unavailable.
                try
                {
                    var ex = GetProcAddress(loaded, (nint)100);
                    if (ex != 0) getStateEx = Marshal.GetDelegateForFunctionPointer<XInputGetStateDelegate>(ex);
                }
                catch { }
                break;
            }
            NativeLibrary.Free(loaded);
        }
    }

    public bool Available => getState is not null;

    public bool SetVibration(int player, ushort lowFrequency, ushort highFrequency)
    {
        if (setState is null || player is < 0 or > 3) return false;
        var v = new XINPUT_VIBRATION { wLeftMotorSpeed = lowFrequency, wRightMotorSpeed = highFrequency };
        return setState((uint)player, ref v) == 0;
    }

    public bool TryRead(int player, out MtGamepadState state)
    {
        state = MtGamepadState.Neutral;
        if (getState is null || player is < 0 or > 3) return false;

        XINPUT_STATE x;
        var rc = getStateEx is not null ? getStateEx((uint)player, out x) : getState((uint)player, out x);
        if (rc != 0 && getStateEx is not null) rc = getState((uint)player, out x);
        if (rc != 0) return false;

        var g = x.Gamepad;
        MtPadButtons b = MtPadButtons.None;
        // XInput -> Nintendo physical-position mapping (Xbox A becomes Switch B, etc.).
        if ((g.wButtons & 0x1000) != 0) b |= MtPadButtons.B;
        if ((g.wButtons & 0x2000) != 0) b |= MtPadButtons.A;
        if ((g.wButtons & 0x4000) != 0) b |= MtPadButtons.Y;
        if ((g.wButtons & 0x8000) != 0) b |= MtPadButtons.X;
        if ((g.wButtons & 0x0100) != 0) b |= MtPadButtons.L;
        if ((g.wButtons & 0x0200) != 0) b |= MtPadButtons.R;
        if (g.bLeftTrigger > 20) b |= MtPadButtons.ZL;
        if (g.bRightTrigger > 20) b |= MtPadButtons.ZR;
        if ((g.wButtons & 0x0020) != 0) b |= MtPadButtons.Minus;
        if ((g.wButtons & 0x0010) != 0) b |= MtPadButtons.Plus;
        if ((g.wButtons & 0x0040) != 0) b |= MtPadButtons.LStick;
        if ((g.wButtons & 0x0080) != 0) b |= MtPadButtons.RStick;
        if ((g.wButtons & 0x0001) != 0) b |= MtPadButtons.Up;
        if ((g.wButtons & 0x0002) != 0) b |= MtPadButtons.Down;
        if ((g.wButtons & 0x0004) != 0) b |= MtPadButtons.Left;
        if ((g.wButtons & 0x0008) != 0) b |= MtPadButtons.Right;
        if ((g.wButtons & 0x0400) != 0) b |= MtPadButtons.Home; // Guide/Xbox button via GetStateEx

        state = new MtGamepadState(b, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY,
            (ushort)(g.bLeftTrigger * 257), (ushort)(g.bRightTrigger * 257));
        return true;
    }
}
