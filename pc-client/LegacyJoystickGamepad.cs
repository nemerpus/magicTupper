using System.Runtime.InteropServices;
using System.Text;

namespace MagicTupperPcClient;

/// <summary>
/// WinMM joystick backend. It complements XInput and is intentionally dependency-free.
/// This covers DirectInput/HID controllers that Windows exposes as legacy joysticks,
/// including DualShock 4 on the normal Windows Bluetooth/USB stack.
/// </summary>
internal sealed class LegacyJoystickGamepad
{
    private const int JoyReturnAll = 0x000000FF;
    private const uint JoyPovCentered = 0xFFFF;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct JOYCAPS
    {
        public ushort wMid, wPid;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
        public uint wXmin, wXmax, wYmin, wYmax, wZmin, wZmax;
        public uint wNumButtons, wPeriodMin, wPeriodMax, wRmin, wRmax, wUmin, wUmax, wVmin, wVmax;
        public uint wCaps, wMaxAxes, wNumAxes, wMaxButtons;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szRegKey;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szOEMVxD;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOYINFOEX
    {
        public uint dwSize, dwFlags;
        public uint dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos;
        public uint dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
    }

    [DllImport("winmm.dll", CharSet = CharSet.Unicode)] private static extern uint joyGetDevCapsW(uint uJoyID, out JOYCAPS pjc, uint cbjc);
    [DllImport("winmm.dll")] private static extern uint joyGetPosEx(uint uJoyID, ref JOYINFOEX pji);
    [DllImport("winmm.dll")] private static extern uint joyGetNumDevs();

    private JOYCAPS caps;
    public string DeviceName { get; private set; } = "Mando HID/DirectInput";

    public bool TryRead(out MtGamepadState state) => TryReadSlot(0, out state, out _);

    public bool TryReadSlot(int slot, out MtGamepadState state, out string deviceName)
    {
        state = MtGamepadState.Neutral;
        deviceName = "Mando HID/DirectInput";
        if (slot is < 0 or > 3) return false;
        var count = Math.Min(joyGetNumDevs(), 16u);
        var found = 0;
        for (uint i = 0; i < count; i++)
        {
            if (joyGetDevCapsW(i, out var c, (uint)Marshal.SizeOf<JOYCAPS>()) != 0) continue;
            caps = c;
            if (!TryReadId(i, out var candidate)) continue;
            if (found++ != slot) continue;
            state = candidate;
            DeviceName = string.IsNullOrWhiteSpace(c.szPname) ? $"Joystick {i + 1}" : c.szPname.Trim();
            deviceName = DeviceName;
            return true;
        }
        return false;
    }

    private bool TryReadId(uint id, out MtGamepadState state)
    {
        state = MtGamepadState.Neutral;
        var j = new JOYINFOEX { dwSize = (uint)Marshal.SizeOf<JOYINFOEX>(), dwFlags = JoyReturnAll };
        if (joyGetPosEx(id, ref j) != 0) return false;

        var sony = DeviceName.Contains("Wireless Controller", StringComparison.OrdinalIgnoreCase) ||
                   caps.szPname?.Contains("Wireless Controller", StringComparison.OrdinalIgnoreCase) == true ||
                   caps.wMid == 0x054C;
        state = sony ? MapDualShock4(j) : MapGeneric(j);
        return true;
    }

    private MtGamepadState MapDualShock4(JOYINFOEX j)
    {
        // DirectInput button order used by the standard DualShock 4 Windows HID driver:
        // 1 Square, 2 Cross, 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2,
        // 9 Share, 10 Options, 11 L3, 12 R3, 13 PS, 14 Touchpad.
        MtPadButtons b = MtPadButtons.None;
        Btn(j, 0, ref b, MtPadButtons.Y); // Square -> Switch Y (left physical button)
        Btn(j, 1, ref b, MtPadButtons.B); // Cross  -> Switch B (bottom)
        Btn(j, 2, ref b, MtPadButtons.A); // Circle -> Switch A (right)
        Btn(j, 3, ref b, MtPadButtons.X); // Triangle -> Switch X (top)
        Btn(j, 4, ref b, MtPadButtons.L);
        Btn(j, 5, ref b, MtPadButtons.R);
        Btn(j, 6, ref b, MtPadButtons.ZL);
        Btn(j, 7, ref b, MtPadButtons.ZR);
        Btn(j, 8, ref b, MtPadButtons.Minus);
        Btn(j, 9, ref b, MtPadButtons.Plus);
        Btn(j, 10, ref b, MtPadButtons.LStick);
        Btn(j, 11, ref b, MtPadButtons.RStick);
        Btn(j, 12, ref b, MtPadButtons.Home);    // PS -> Switch HOME
        Btn(j, 13, ref b, MtPadButtons.Capture); // Touchpad click -> Switch Capture
        MapPov(j.dwPOV, ref b);
        // WinMM commonly exposes DS4 X/Y + Z/R for the two sticks. U/V may vary by driver.
        return new MtGamepadState(b,
            Axis(j.dwXpos, caps.wXmin, caps.wXmax, false), Axis(j.dwYpos, caps.wYmin, caps.wYmax, true),
            Axis(j.dwZpos, caps.wZmin, caps.wZmax, false), Axis(j.dwRpos, caps.wRmin, caps.wRmax, true),
            ButtonTrigger(j.dwButtons, 6), ButtonTrigger(j.dwButtons, 7));
    }

    private MtGamepadState MapGeneric(JOYINFOEX j)
    {
        // Sensible physical-position default. A future profile editor can override this per device.
        MtPadButtons b = MtPadButtons.None;
        Btn(j, 0, ref b, MtPadButtons.B); Btn(j, 1, ref b, MtPadButtons.A);
        Btn(j, 2, ref b, MtPadButtons.Y); Btn(j, 3, ref b, MtPadButtons.X);
        Btn(j, 4, ref b, MtPadButtons.L); Btn(j, 5, ref b, MtPadButtons.R);
        Btn(j, 6, ref b, MtPadButtons.ZL); Btn(j, 7, ref b, MtPadButtons.ZR);
        Btn(j, 8, ref b, MtPadButtons.Minus); Btn(j, 9, ref b, MtPadButtons.Plus);
        Btn(j, 10, ref b, MtPadButtons.LStick); Btn(j, 11, ref b, MtPadButtons.RStick);
        // Many DirectInput pads expose their Guide/Home and auxiliary key as buttons 13/14.
        // When present, map them to Switch HOME/Capture. Profiles can refine this later.
        if (caps.wNumButtons >= 13) Btn(j, 12, ref b, MtPadButtons.Home);
        if (caps.wNumButtons >= 14) Btn(j, 13, ref b, MtPadButtons.Capture);
        MapPov(j.dwPOV, ref b);
        return new MtGamepadState(b,
            Axis(j.dwXpos, caps.wXmin, caps.wXmax, false), Axis(j.dwYpos, caps.wYmin, caps.wYmax, true),
            Axis(j.dwRpos, caps.wRmin, caps.wRmax, false), Axis(j.dwUpos, caps.wUmin, caps.wUmax, true),
            ButtonTrigger(j.dwButtons, 6), ButtonTrigger(j.dwButtons, 7));
    }

    private static void Btn(JOYINFOEX j, int index, ref MtPadButtons value, MtPadButtons flag)
    { if ((j.dwButtons & (1u << index)) != 0) value |= flag; }

    private static ushort ButtonTrigger(uint buttons, int index) => (buttons & (1u << index)) != 0 ? ushort.MaxValue : (ushort)0;

    private static short Axis(uint value, uint min, uint max, bool invert)
    {
        if (max <= min) { min = 0; max = 65535; }
        var norm = ((double)value - min) / (max - min);
        norm = Math.Clamp(norm, 0, 1) * 2.0 - 1.0;
        if (Math.Abs(norm) < 0.06) norm = 0; // light deadzone for noisy HID pads
        if (invert) norm = -norm;
        return (short)Math.Clamp((int)Math.Round(norm * 32767.0), short.MinValue + 1, short.MaxValue);
    }

    private static void MapPov(uint pov, ref MtPadButtons b)
    {
        if (pov == JoyPovCentered || pov > 35999) return;
        var deg = pov / 100.0;
        if (deg >= 315 || deg < 45) b |= MtPadButtons.Up;
        if (deg >= 45 && deg < 135) b |= MtPadButtons.Right;
        if (deg >= 135 && deg < 225) b |= MtPadButtons.Down;
        if (deg >= 225 && deg < 315) b |= MtPadButtons.Left;
    }
}
