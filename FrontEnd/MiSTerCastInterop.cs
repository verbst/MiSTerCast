using System;
using System.Runtime.InteropServices;

namespace MiSTerCast
{
    class MiSTerCastInterop
    {
        /*
        int ALIGMENT_CENTER = 0;
        int ALIGMENT_TOP_LEFT = 1;
        int ALIGMENT_TOP = 2;
        int ALIGMENT_TOP_RIGHT = 3;
        int ALIGMENT_RIGHT = 4;
        int ALIGMENT_BOTTOM_RIGHT = 5;
        int ALIGMENT_BOTTOM = 6;
        int ALIGMENT_BOTTOM_LEFT = 7;
        int ALIGMENT_LEFT = 8;

        int ROTATE_NONE = 0;
        int ROTATE_CCW =  1;
        int ROTATE_CW =   2;
        int ROTATE_180 =  3;
        */

        // Mirrors ModelineValidation in MiSTerCastLib.h.
        public enum ModelineValidation : int
        {
            Ok = 0,
            Malformed = 1,
            OverByteBudget = 2,
            OverCrtEnvelope = 3
        }

        // Mirrors Codec in MiSTerCastLib.h (CMD_INIT byte[1]).
        public enum Codec : byte
        {
            Raw = 0,
            LZ4 = 1,
            LZ4Delta = 2,
            LZ4HC = 3,
            LZ4HCDelta = 4,
            LZ4Adaptive = 5,
            LZ4AdaptiveDelta = 6,
            NLC = 7
        }

        public enum NlcPack : byte
        {
            Tiled = 1,
            Rice = 2
        }

        public enum RgbMode : byte
        {
            Rgb888 = 0,
            Rgba8888 = 1,
            Rgb565 = 2
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void LogDelegate(string message, bool error);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void CaptureImageDelegate(int width, int height, IntPtr buffer);

        // Note on marshalling: the native side uses C++ `bool` (one byte), while
        // .NET's default for `bool` is the four-byte Win32 BOOL. Pinning both
        // arguments and returns to UnmanagedType.I1 keeps the two in step - it is
        // only free by accident on x86, and this now builds for x64 as well.

        [DllImport("MISTERCASTLIB.dll", EntryPoint = "Initialize", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool Initialize(LogDelegate logCallback, CaptureImageDelegate captureImageCallback);

        [DllImport("MISTERCASTLIB.dll", EntryPoint = "Shutdown", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool Shutdown();

        [DllImport("MISTERCASTLIB.dll", EntryPoint = "StartStream", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool StartStream(string targetIp);

        [DllImport("MISTERCASTLIB.dll", EntryPoint = "StopStream", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool StopStream();

        // Applied at the next StartStream: codec, RGB mode and MTU ride CMD_INIT
        // and cannot be changed on a live session.
        [DllImport("MISTERCASTLIB.dll", EntryPoint = "SetStreamOptions", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SetStreamOptions(
            byte codec,
            byte nlcPack,
            byte nearLevel,
            byte rgbMode,
            UInt16 mtu,
            [MarshalAs(UnmanagedType.I1)] bool autoReconnect,
            byte verbose,
            [MarshalAs(UnmanagedType.I1)] bool allowOversizeModes);

        // Pure check, for gating the UI before streaming. Returns ModelineValidation.
        [DllImport("MISTERCASTLIB.dll", EntryPoint = "ValidateModeline", CallingConvention = CallingConvention.Cdecl)]
        public static extern int ValidateModeline(
            Double pclock,
            UInt16 hactive,
            UInt16 hbegin,
            UInt16 hend,
            UInt16 htotal,
            UInt16 vactive,
            UInt16 vbegin,
            UInt16 vend,
            UInt16 vtotal,
            [MarshalAs(UnmanagedType.I1)] bool interlace,
            byte rgbMode,
            [MarshalAs(UnmanagedType.I1)] bool allowOversizeModes);

        [DllImport("MISTERCASTLIB.dll", EntryPoint = "SetModeline", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SetModeline(
            Double pclock,
            UInt16 hactive,
            UInt16 hbegin,
            UInt16 hend,
            UInt16 htotal,
            UInt16 vactive,
            UInt16 vbegin,
            UInt16 vend,
            UInt16 vtotal,
            [MarshalAs(UnmanagedType.I1)] bool interlace);

        [DllImport("MISTERCASTLIB.dll", EntryPoint = "SetSource", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SetSource(
            byte display,
            [MarshalAs(UnmanagedType.I1)] bool audio,
            [MarshalAs(UnmanagedType.I1)] bool preview,
            byte alignment,
            byte cropmode,
            UInt16 width,
            UInt16 height,
            Int16 xoffset,
            Int16 yoffset,
            byte rotation);

        // windowHandle == IntPtr.Zero releases single-window capture and
        // returns to capturing the display set by SetSource.
        [DllImport("MISTERCASTLIB.dll", EntryPoint = "SetCaptureWindow", CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SetCaptureWindow(IntPtr windowHandle);
    }
}
