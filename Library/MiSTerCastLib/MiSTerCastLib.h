#ifdef MISTERCASTLIB_EXPORTS
#define MISTERCASTLIB_API extern "C" __declspec(dllexport)
#else
#define MISTERCASTLIB_API __declspec(dllimport)
#endif

void LogMessage(std::string message, bool error = false);

#define EXIT_ON_ERROR(hres, message)  \
              if (FAILED(hres)){\
                LogMessage(std::string(message) + ": " + std::to_string(hres), true);\
                return false;}
#define SAFE_RELEASE(punk)  \
              if ((punk) != NULL)  \
                { (punk)->Release(); (punk) = NULL; }

enum Alignment : int
{
    Center,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left
};

enum CropMode : int
{
    Custom,
    X1,
    X2,
    X3,
    X4,
    X5,
    Full43,
    Full54,
    FullSource,
};

enum Rotation : int
{
    None,
    CW90,
    CCW90,
    Flip180
};

// CMD_INIT byte[4]. Determines bytes-per-pixel, and so the modeline byte budget.
enum RgbMode : int
{
    Rgb888 = 0,   // 3 bytes/px
    Rgba8888 = 1, // 4 bytes/px
    Rgb565 = 2    // 2 bytes/px
};

// CMD_INIT byte[1] codec field. See the Groovy integration handoff section 5.1.
enum Codec : int
{
    CodecRaw = 0,
    CodecLZ4 = 1,
    CodecLZ4Delta = 2,
    CodecLZ4HC = 3,
    CodecLZ4HCDelta = 4,
    CodecLZ4Adaptive = 5,
    CodecLZ4AdaptiveDelta = 6,
    CodecNLC = 7
};

// NLC entropy front-end (setNlcPack). Rice needs a Rice-capable core: an older
// one misparses Rice bytes as TILED and shows garbage with no error at all.
enum NlcPack : int
{
    NlcPackTiled = 1,
    NlcPackRice = 2
};

// Result of ValidateModeline / SetModeline. Anything non-zero refuses the mode.
enum ModelineValidation : int
{
    ModelineOk = 0,
    ModelineMalformed = 1,        // pclock <= 0, zero active area, blanking not enclosing
    ModelineOverByteBudget = 2,   // hactive*vactive*bpp exceeds the client's frame buffer
    ModelineOverCrtEnvelope = 3   // beyond what a fixed-frequency CRT should be asked to sync
};

// Everything here rides CMD_INIT and cannot change mid-session, so these are
// applied at the next StartStream rather than live.
struct StreamOptions {
    UINT8  codec;
    UINT8  nlcPack;             // ignored unless codec == CodecNLC
    UINT8  nearLevel;           // 0-3, ignored unless codec == CodecNLC
    UINT8  rgbMode;
    UINT16 mtu;                 // 1500, or 3800 with the core's Jumbo frames option on
    bool   autoReconnect;
    UINT8  verbose;             // GroovyMister log verbosity, 0-2
    bool   allowOversizeModes;  // bypass the CRT envelope check (not the byte budget)
};

struct SourceOptions {
    bool syncrefresh;
    UINT16 framedelay;
    UINT8 display;
    bool audio;
    bool preview;
    Alignment alignment;
    CropMode cropmode;
    UINT16 width;
    UINT16 height;
    INT16 xoffset;
    INT16 yoffset;
    UINT8 rotation;
    UINT_PTR windowHandle;  // 0 = capture the display in `display`; otherwise capture this window
};

typedef void(__stdcall *log_function)(const char* message, bool error);

typedef void(__stdcall *capture_image_function)(int width, int height, void* buffer);

MISTERCASTLIB_API bool Initialize(log_function fnLog, capture_image_function fnCapture);

MISTERCASTLIB_API bool Shutdown();

MISTERCASTLIB_API bool StartStream(const char* targetIp);

MISTERCASTLIB_API bool StopStream();

// Applies at the next StartStream. Codec, RGB mode and MTU ride CMD_INIT and
// cannot be changed on a live session.
MISTERCASTLIB_API bool SetStreamOptions(
    UINT8 codec,
    UINT8 nlcPack,
    UINT8 nearLevel,
    UINT8 rgbMode,
    UINT16 mtu,
    bool autoReconnect,
    UINT8 verbose,
    bool allowOversizeModes);

// Pure check, so the UI can gate before streaming. Returns a ModelineValidation.
// Takes rgbMode explicitly because the budget is in bytes, not pixels.
MISTERCASTLIB_API int ValidateModeline(
    double pclock,
    UINT16 hactive,
    UINT16 hbegin,
    UINT16 hend,
    UINT16 htotal,
    UINT16 vactive,
    UINT16 vbegin,
    UINT16 vend,
    UINT16 vtotal,
    bool interlace,
    UINT8 rgbMode,
    bool allowOversizeModes);

// Returns false and leaves the previous mode in force if the modeline does not
// validate against the current stream options.
MISTERCASTLIB_API bool SetModeline(
    double pclock,
    UINT16 hactive,
    UINT16 hbegin,
    UINT16 hend,
    UINT16 htotal,
    UINT16 vactive,
    UINT16 vbegin,
    UINT16 vend,
    UINT16 vtotal,
    bool interlace);

MISTERCASTLIB_API bool SetSource(
    UINT8 display,
    bool audio,
    bool preview,
    UINT8 alignment,
    UINT8 cropmode,
    UINT16 width,
    UINT16 height,
    INT16 xoffset,
    INT16 yoffset,
    UINT8 rotation);

// windowHandle == 0 releases single-window capture and returns to capturing
// the display set by SetSource. Requires Windows 10 version 1903 or newer.
MISTERCASTLIB_API bool SetCaptureWindow(UINT_PTR windowHandle);
