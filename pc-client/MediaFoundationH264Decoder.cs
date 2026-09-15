using MediaFoundation;
using MediaFoundation.Transform;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace MagicTupperPcClient;

/// <summary>Native Windows Media Foundation H.264 MFT. No external ffplay/FFmpeg process.</summary>
internal sealed class MediaFoundationH264Decoder : IDisposable
{
    private readonly IMFTransform decoder;
    private int width;
    private int height;
    private bool disposed;
    private long sampleTime;
    private const long FrameDuration100ns = 10_000_000 / 30;
    private const int MfTransformStreamChange = unchecked((int)0xC00D6D61);
    private const int MfTransformNeedMoreInput = unchecked((int)0xC00D6D72);
    private static readonly Guid H264 = new("34363248-0000-0010-8000-00AA00389B71");
    private static readonly Guid NV12 = new("3231564E-0000-0010-8000-00AA00389B71");
    private static readonly Guid Video = new("73646976-0000-0010-8000-00AA00389B71");
    private static readonly Guid MfLowLatency = new("9c27891a-ed7a-40e1-88e8-b22727a024ee");

    public event Action<string>? Status;
    public int Width => width;
    public int Height => height;

    public MediaFoundationH264Decoder(int width = 1280, int height = 720)
    {
        this.width = width; this.height = height;
        Hr(MFExtern.MFStartup(0x20070, MFStartup.Full), "MFStartup");
        decoder = (IMFTransform)new MSH264DecoderMFT();
        if (decoder.GetAttributes(out var attrs) == HResult.S_OK)
        {
            try { attrs.SetUINT32(MfLowLatency, 1); } finally { Release(attrs); }
        }
        Hr(MFExtern.MFCreateMediaType(out var input), "MFCreateMediaType(input)");
        try
        {
            Hr(input.SetGUID(MFAttributesClsid.MF_MT_MAJOR_TYPE, Video), "input major");
            Hr(input.SetGUID(MFAttributesClsid.MF_MT_SUBTYPE, H264), "input H264");
            Hr(MFExtern.MFSetAttributeSize(input, MFAttributesClsid.MF_MT_FRAME_SIZE, width, height), "input size");
            Hr(MFExtern.MFSetAttributeRatio(input, MFAttributesClsid.MF_MT_FRAME_RATE, 30, 1), "input fps");
            Hr(decoder.SetInputType(0, input, MFTSetTypeFlags.None), "SetInputType H264");
        }
        finally { Release(input); }

        // Set a provisional output format. The Microsoft H.264 decoder can request a
        // stream change after parsing SPS/PPS; DrainOneFrame() renegotiates then.
        Hr(MFExtern.MFCreateMediaType(out var output), "MFCreateMediaType(output)");
        try
        {
            Hr(output.SetGUID(MFAttributesClsid.MF_MT_MAJOR_TYPE, Video), "output major");
            Hr(output.SetGUID(MFAttributesClsid.MF_MT_SUBTYPE, NV12), "output NV12");
            Hr(MFExtern.MFSetAttributeSize(output, MFAttributesClsid.MF_MT_FRAME_SIZE, width, height), "output size");
            Hr(MFExtern.MFSetAttributeRatio(output, MFAttributesClsid.MF_MT_FRAME_RATE, 30, 1), "output fps");
            Hr(decoder.SetOutputType(0, output, MFTSetTypeFlags.None), "SetOutputType NV12");
        }
        finally { Release(output); }
        decoder.ProcessMessage(MFTMessageType.NotifyBeginStreaming, IntPtr.Zero);
        decoder.ProcessMessage(MFTMessageType.NotifyStartOfStream, IntPtr.Zero);
    }

    public Bitmap? DecodeAccessUnit(ReadOnlySpan<byte> annexB)
    {
        if (disposed || annexB.IsEmpty) return null;
        Hr(MFExtern.MFCreateSample(out var sample), "MFCreateSample");
        IMFMediaBuffer? inBuffer = null;
        try
        {
            Hr(MFExtern.MFCreateMemoryBuffer(annexB.Length, out inBuffer), "MFCreateMemoryBuffer");
            Hr(inBuffer.Lock(out var ptr, out _, out _), "input Lock");
            try { Marshal.Copy(annexB.ToArray(), 0, ptr, annexB.Length); }
            finally { inBuffer.Unlock(); }
            Hr(inBuffer.SetCurrentLength(annexB.Length), "input length");
            Hr(sample.AddBuffer(inBuffer), "AddBuffer");
            sample.SetSampleTime(sampleTime); sample.SetSampleDuration(FrameDuration100ns); sampleTime += FrameDuration100ns;
            var pi = decoder.ProcessInput(0, sample, 0);
            if (pi != HResult.S_OK) return null;
        }
        finally { Release(inBuffer); Release(sample); }
        return DrainOneFrame();
    }

    private Bitmap? DrainOneFrame()
    {
        // A stream-change invalidates output stream info/sample requirements. Re-query
        // and recreate all output objects after each renegotiation attempt.
        for (int attempt = 0; attempt < 4; attempt++)
        {
            Hr(decoder.GetOutputStreamInfo(0, out var info), "GetOutputStreamInfo");
            IMFSample? outSample = null;
            IMFMediaBuffer? outBuffer = null;
            IntPtr samplePtr = IntPtr.Zero;
            try
            {
                bool decoderProvides = (info.dwFlags & MFTOutputStreamInfoFlags.ProvidesSamples) != 0;
                if (!decoderProvides)
                {
                    Hr(MFExtern.MFCreateSample(out outSample), "MFCreateSample(output)");
                    var bytes = Math.Max(info.cbSize, checked(width * height * 3 / 2));
                    Hr(MFExtern.MFCreateMemoryBuffer(bytes, out outBuffer), "MFCreateMemoryBuffer(output)");
                    Hr(outSample.AddBuffer(outBuffer), "AddBuffer(output)");
                    samplePtr = Marshal.GetIUnknownForObject(outSample);
                }

                var data = new[] { new MFTOutputDataBuffer { dwStreamID = 0, pSample = samplePtr } };
                var hr = decoder.ProcessOutput(MFTProcessOutputFlags.None, 1, data, out _);
                int hrCode = (int)hr;

                if (hrCode == MfTransformNeedMoreInput)
                    return null;

                if (hrCode == MfTransformStreamChange)
                {
                    Status?.Invoke("MF: STREAM_CHANGE detectado · renegociando salida…");
                    RenegotiateOutput();
                    continue;
                }

                Hr(hr, "ProcessOutput");

                IMFSample actual;
                if (data[0].pSample != IntPtr.Zero && data[0].pSample != samplePtr)
                    actual = (IMFSample)Marshal.GetObjectForIUnknown(data[0].pSample);
                else if (outSample is not null)
                    actual = outSample;
                else
                    return null;

                try
                {
                    Hr(actual.ConvertToContiguousBuffer(out var contiguous), "ConvertToContiguousBuffer");
                    try
                    {
                        Hr(contiguous.Lock(out var p, out _, out var current), "NV12 Lock");
                        try { return Nv12ToBitmap(p, current); }
                        finally { contiguous.Unlock(); }
                    }
                    finally { Release(contiguous); }
                }
                finally
                {
                    if (outSample is null || !ReferenceEquals(actual, outSample)) Release(actual);
                }
            }
            finally
            {
                if (samplePtr != IntPtr.Zero) Marshal.Release(samplePtr);
                Release(outBuffer);
                Release(outSample);
            }
        }

        throw new InvalidOperationException("Media Foundation solicitó demasiados cambios de secuencia consecutivos.");
    }

    private void RenegotiateOutput()
    {
        IMFMediaType? chosen = null;
        IMFMediaType? fallback = null;
        Guid chosenSubtype = Guid.Empty;

        try
        {
            // Enumerate the transform's updated output types after it has parsed SPS/PPS.
            // Prefer NV12 because the renderer below consumes NV12 directly.
            for (int index = 0; index < 64; index++)
            {
                IMFMediaType? type = null;
                var hr = decoder.GetOutputAvailableType(0, index, out type);
                if ((int)hr < 0 || type is null) break;

                Guid subtype = Guid.Empty;
                try { type.GetGUID(MFAttributesClsid.MF_MT_SUBTYPE, out subtype); } catch { }

                if (fallback is null)
                {
                    fallback = type;
                    type = null;
                }

                if (subtype == NV12)
                {
                    chosen = type ?? fallback;
                    chosenSubtype = subtype;
                    if (ReferenceEquals(chosen, fallback)) fallback = null;
                    type = null;
                    break;
                }

                Release(type);
            }

            // Microsoft H.264 MFT normally exposes NV12. If it does not, using an
            // arbitrary fallback would make Nv12ToBitmap interpret another pixel
            // format incorrectly, so fail with a precise diagnostic instead.
            if (chosen is null)
            {
                if (fallback is not null)
                {
                    try { fallback.GetGUID(MFAttributesClsid.MF_MT_SUBTYPE, out chosenSubtype); } catch { }
                }
                throw new NotSupportedException($"El decoder H.264 no ofrece NV12 tras STREAM_CHANGE (primer subtipo: {FormatGuid(chosenSubtype)}).");
            }

            Hr(decoder.SetOutputType(0, chosen, MFTSetTypeFlags.None), "SetOutputType after STREAM_CHANGE");

            int newWidth = width, newHeight = height;
            var sizeHr = MFExtern.MFGetAttributeSize(chosen, MFAttributesClsid.MF_MT_FRAME_SIZE, out newWidth, out newHeight);
            if ((int)sizeHr >= 0 && newWidth > 0 && newHeight > 0)
            {
                width = newWidth;
                height = newHeight;
            }

            Status?.Invoke($"MF: STREAM_CHANGE → NV12 {width}×{height} · salida renegociada ✓");
        }
        finally
        {
            Release(chosen);
            Release(fallback);
        }
    }

    private unsafe Bitmap Nv12ToBitmap(IntPtr source, int length)
    {
        int required = checked(width * height * 3 / 2);
        if (length < required) throw new InvalidDataException($"NV12 corto: {length} bytes; esperado al menos {required} para {width}x{height}");
        var bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb);
        var bits = bmp.LockBits(new Rectangle(0,0,width,height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
        try
        {
            byte* src=(byte*)source; byte* yPlane=src; byte* uvPlane=src+width*height;
            for(int y=0;y<height;y++)
            {
                byte* dst=(byte*)bits.Scan0 + y*bits.Stride; byte* uv=uvPlane+(y>>1)*width;
                for(int x=0;x<width;x++)
                {
                    int Y=yPlane[y*width+x]; int U=uv[x&~1]-128; int V=uv[(x&~1)+1]-128;
                    int c=Y-16; if(c<0)c=0;
                    int r=(298*c+409*V+128)>>8, g=(298*c-100*U-208*V+128)>>8, b=(298*c+516*U+128)>>8;
                    dst[x*3+0]=(byte)Math.Clamp(b,0,255); dst[x*3+1]=(byte)Math.Clamp(g,0,255); dst[x*3+2]=(byte)Math.Clamp(r,0,255);
                }
            }
        }
        finally { bmp.UnlockBits(bits); }
        return bmp;
    }

    private static string FormatGuid(Guid guid) => guid == Guid.Empty ? "desconocido" : guid.ToString("D");
    private static void Hr(HResult hr, string op)
    {
        if ((int)hr < 0)
            throw new COMException($"{op} falló (0x{((int)hr):X8}).", (int)hr);
    }
    private static void Release(object? o) { if (o is not null && Marshal.IsComObject(o)) try { Marshal.ReleaseComObject(o); } catch { } }
    public void Dispose()
    {
        if(disposed)return; disposed=true;
        try { decoder.ProcessMessage(MFTMessageType.NotifyEndOfStream, IntPtr.Zero); decoder.ProcessMessage(MFTMessageType.NotifyEndStreaming, IntPtr.Zero); } catch{}
        Release(decoder); try { MFExtern.MFShutdown(); } catch{}
    }
}
