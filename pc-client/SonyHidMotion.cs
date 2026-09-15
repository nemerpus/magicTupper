using HidSharp;
using System.Buffers.Binary;

namespace MagicTupperPcClient;

/// <summary>Raw Sony HID six-axis reader. Buttons continue through XInput/WinMM; this class overlays motion.</summary>
internal sealed class SonyHidMotion : IDisposable
{
    private sealed class Sensor : IDisposable
    {
        public readonly string Name;
        public readonly HidDevice Device;
        public readonly HidStream Stream;
        public readonly CancellationTokenSource Cts = new();
        public readonly object Gate = new();
        public MtMotionState State;
        private float ax, ay, az;
        private long lastTicks;
        private byte lastReportId;

        public Sensor(HidDevice device, HidStream stream)
        {
            Device = device; Stream = stream; Name = device.GetFriendlyName() ?? "Sony HID";
            _ = Task.Run(ReadLoop);
        }

        private void ReadLoop()
        {
            var b = new byte[Math.Max(64, Device.GetMaxInputReportLength())];
            while (!Cts.IsCancellationRequested)
            {
                try
                {
                    int n = Stream.Read(b, 0, b.Length);
                    if (n < 27) continue;
                    lastReportId = b[0];
                    bool ds5 = Device.ProductID is 0x0CE6 or 0x0DF2;
                    bool bt = b[0] is 0x11 or 0x31;
                    int gyro = ds5 ? (bt ? 17 : 16) : (bt ? 15 : 13);
                    int accel = ds5 ? (bt ? 23 : 22) : (bt ? 21 : 19);
                    if (gyro + 5 >= n || accel + 5 >= n) continue;
                    short gx = I16(b, gyro), gy = I16(b, gyro+2), gz = I16(b, gyro+4);
                    short acx = I16(b, accel), acy = I16(b, accel+2), acz = I16(b, accel+4);
                    var now = System.Diagnostics.Stopwatch.GetTimestamp();
                    float dt = lastTicks == 0 ? 0 : (float)(now-lastTicks) / System.Diagnostics.Stopwatch.Frequency;
                    lastTicks = now;
                    if (dt > 0 && dt < 0.1f)
                    {
                        // Sony raw gyro is approximately 1024 counts/(deg/s). Integrate to radians.
                        const float k = MathF.PI / 180f / 1024f;
                        ax += gx * k * dt; ay += gy * k * dt; az += gz * k * dt;
                    }
                    var m = new MtMotionState(ax, ay, az, acx/8192f, acy/8192f, acz/8192f, true);
                    lock (Gate) State = m;
                }
                catch (TimeoutException) { }
                catch { if (!Cts.IsCancellationRequested) Thread.Sleep(20); }
            }
        }
        private static short I16(byte[] b, int o) => BinaryPrimitives.ReadInt16LittleEndian(b.AsSpan(o,2));
        public bool SetUsbDs4Rumble(byte small, byte big)
        {
            if (Device.ProductID is not (0x05C4 or 0x09CC or 0x0BA0) || lastReportId != 0x01) return false;
            try
            {
                var report = new byte[Math.Max(32, Device.GetMaxOutputReportLength())];
                report[0]=0x05; report[1]=0xFF; report[4]=small; report[5]=big; report[8]=0xFF;
                lock (Gate) { Stream.Write(report,0,report.Length); Stream.Flush(); }
                return true;
            }
            catch { return false; }
        }
        public void Dispose() { Cts.Cancel(); try { Stream.Dispose(); } catch{} Cts.Dispose(); }
    }

    private readonly List<Sensor> sensors = new();
    public SonyHidMotion()
    {
        try
        {
            foreach (var d in DeviceList.Local.GetHidDevices(0x054C).Take(4))
            {
                try
                {
                    var s = d.Open(); s.ReadTimeout = 250;
                    sensors.Add(new Sensor(d, s));
                }
                catch { }
            }
        }
        catch { }
    }

    public bool TryRead(int slot, out MtMotionState state, out string name)
    {
        state = MtMotionState.None; name = "";
        if (slot < 0 || slot >= sensors.Count) return false;
        var s = sensors[slot]; lock (s.Gate) state = s.State; name = s.Name;
        return state.Valid;
    }

    public bool SetRumble(int slot, byte small, byte big)
    {
        if (slot < 0 || slot >= sensors.Count) return false;
        return sensors[slot].SetUsbDs4Rumble(small,big);
    }

    public void Dispose() { foreach (var s in sensors) s.Dispose(); sensors.Clear(); }
}
