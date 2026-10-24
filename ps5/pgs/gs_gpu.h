#pragma once

// The GS drawn on the graphics card (paraLLEl-GS, Vulkan compute), next to the CPU renderer.
// This header carries no Vulkan / Granite types: only gs_gpu_parallel.cpp sees the library.
//
// VENDETTA_GS=gpu   (default) the graphics card draws everything; nothing is drawn on the CPU (reads of GS memory are
//                   answered by the graphics card). Without a usable Vulkan device: the CPU renderer, as with cpu.
// VENDETTA_GS=cpu   the CPU renderer alone
// VENDETTA_GS=both  both draw: the CPU renderer answers the reads; the window shows the graphics card's picture
//                   (VENDETTA_GS_SHOW=cpu: the CPU renderer's, for comparing)
//
// VENDETTA_SSAA=1|2|4|8|16 (default 8)      samples per pixel (super-sampling)
// VENDETTA_SSAA_ORDERED=0|1 (default 1)     ordered or rotated sample grid
// VENDETTA_SSAA_TEXTURES=0|1 (default 0)    render targets used as textures keep their samples
// VENDETTA_HIRES=0|1 (default 1)            a double-resolution picture from the samples (needs SSAA >= 4)
// VENDETTA_GPU=<index or name substring>    which Vulkan device (default: the library's choice, a discrete GPU first)
// VENDETTA_FILTER=fsr|bilinear|nearest      how the picture is scaled to the window (default fsr on the graphics card:
//                                           AMD FSR 1 upscaling + RCAS sharpening at the window's size; bilinear on the CPU)
// VENDETTA_SHARPEN=<0..2> (default 0.5)     FSR's sharpening: 0 = sharpest, each 1 halves it
// VENDETTA_GPU_LOG=1                        the library's own information lines (devices, extensions)
// VENDETTA_GPU_SYNC=1                       wait for the graphics card thread after every call (debugging)
// VENDETTA_GPU_VERIFY=1[,N]                 (with VENDETTA_GS=both) compare GS memory and the picture every N presentations
//                                           (default 60); VENDETTA_GPU_VERIFY_DUMP=<dir> saves the first 3 differing pairs

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class GSRendererMode : uint8_t
{
    Cpu,
    Gpu,
    Both,
};

// How the picture is scaled to the window. Fsr = on the graphics card (its picture only; else Bilinear).
enum class GSPresentFilter : uint8_t
{
    Nearest,
    Bilinear,
    Fsr,
};

GSRendererMode gsRendererModeFromEnvironment();
GSPresentFilter gsPresentFilterFromEnvironment();
const char *gsRendererModeName(GSRendererMode mode);

struct GSGpuOptions
{
    uint32_t superSampling = 8; // 1, 2, 4, 8 or 16
    bool orderedGrid = true;
    bool superSampledTextures = false;
    bool highResolutionScanout = true;
    bool fsr = true;
    float sharpness = 0.5f;
    bool libraryLog = false;
    bool sync = false;
    size_t ringBytes = size_t{16} << 20u;
    // Pictures handed over in the buffer they were read back into (GSGpuFrame::pixels, alpha as drawn) instead of
    // copied into GSGpuFrame::rgba with alpha made opaque: one copy fewer on the graphics card thread, for a reader
    // that copies the picture on anyway (Crux2).
    bool mappedFrames = false;
    // With mappedFrames: each picture is read back into host memory this renderer allocated itself (anonymous, page
    // aligned) and imported into its device (VK_EXT_external_memory_host), so a reader with a Vulkan device of its own
    // can import the same memory and copy the picture on its graphics card (GSGpuFrame::hostPointer): no processor
    // copy. Off where the device cannot import host memory (said once). Crux2 on the PS5.
    bool hostSharedFrames = false;
    // The graphics card's time measured with its timestamps (GSGpuStatus::gpuMsPerFrame): a query per pass.
    bool gpuTimestamps = false;
    std::string gpuSelector;

    static GSGpuOptions fromEnvironment();
};

// How the picture is scanned out (Crux2's display settings), changed at any time: taken at the next presentation.
struct GSGpuScanout
{
    bool forceProgressive = true; // a progressive picture where the game's mode allows (INT=1, FFMD=0) instead of fields
    bool antiBlur = true;         // counteract the field blending (the blur) some games scan out with
    bool overscan = false;        // the overscan area too (black borders)
    bool crtcOffsets = false;     // the CRTC's offsets honoured (small shifts, borders) instead of hidden
    bool mipmaps = true;          // the game's mipmaps read (off: always the full texture, more detail, can break games)
    bool sharp2D = false;         // sprites and flat UI super-sampled like the 3-D
    bool backbufferPromotion = false; // a final blit's source shown instead of its blurred/dithered copy (a hack)
    // A read of GS memory the graphics card has not finished drawing answers zeros instead of waiting for it
    // (paraLLEl-GS's unsynced read-backs): faster where a game reads its picture back every frame, wrong where
    // it uses what it read. Off: every read waits and is right.
    bool unsyncedReadbacks = false;
    bool dither = true;           // the GS's dithering as the PS2 draws it (off: smooth gradients, no dot pattern)
    bool eagerSubmit = true;      // recorded work handed to the graphics card as it goes (off: at a frame's end and at reads)
};

// One finished picture, read back from the graphics card. 0 × 0 = nothing displayed (black).
struct GSGpuFrame
{
    uint64_t seq = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // For the aspect ratio: the part of the video mode the picture covers, in single-sample pixels.
    uint32_t internalWidth = 0;
    uint32_t internalHeight = 0;
    uint32_t modeWidth = 0;
    uint32_t modeHeight = 0;
    bool highResolution = false;
    std::vector<uint8_t> rgba;
    // GSGpuOptions::mappedFrames: the picture (width x height, R8G8B8A8, alpha as the GS left it), valid while the
    // reader holds this frame (until its next take); rgba stays empty.
    const uint8_t *pixels = nullptr;
    // GSGpuOptions::hostSharedFrames: the memory pixels is in, for importing into another Vulkan device: hostBytes
    // bytes from hostPointer (both multiples of the device's minImportedHostPointerAlignment). It is buffer hostIndex
    // (0-31) of the renderer's ring, allocation hostId (never reused), of generation hostGeneration (raised when the
    // ring is made again at a larger size). The memory stays mapped until the reader has said, through
    // setHostReleased(), that it holds nothing of that generation; and buffer hostIndex is not written again while its
    // bit is set through setHostReading(). Null when this picture is in an ordinary read-back buffer (pixels only).
    const uint8_t *hostPointer = nullptr;
    size_t hostBytes = 0;
    int hostIndex = -1;
    uint64_t hostId = 0;
    uint64_t hostGeneration = 0;
    // Shown straight from the graphics card (VENDETTA_PRESENT=vulkan): the picture is in shared image sharedImage
    // (R8G8B8A8, width x height) of the current set (Windows handles sharedHandles, made again when sharedGeneration
    // changes), finished once the shared timeline semaphore (sharedTimeline) reaches seq; rgba stays empty. The
    // device's UUID (sharedDeviceUuid): the window's device must be on the same graphics card.
    bool shared = false;
    int sharedImage = -1;
    uint64_t sharedGeneration = 0;
    void *sharedHandles[4] = {};
    void *sharedTimeline = nullptr;
    uint8_t sharedDeviceUuid[16] = {};
};

// What the graphics card renderer is doing (the debug panel's Graphics tab).
struct GSGpuStatus
{
    uint32_t superSampling = 0;
    bool highResolution = false;
    bool fsr = false;
    float sharpness = 0.0f;
    uint32_t pictureWidth = 0; // the last picture read back
    uint32_t pictureHeight = 0;
    float busyPercent = 0.0f; // the graphics card thread, over the last second
    float presentsPerSecond = 0.0f;
    float gifMegabytesPerSecond = 0.0f;
    float renderPassesPerSecond = 0.0f;
    // Where the graphics card thread's time went over the last second (percent of the second): taking in the
    // GS's input (turning it into work), flushing at a picture, scanning it out, and reading it back.
    float gifPercent = 0.0f;
    float flushPercent = 0.0f;
    float vsyncPercent = 0.0f;
    float readbackPercent = 0.0f;
    // The graphics card's own time, from its timestamps (GSGpuOptions::gpuTimestamps), as milliseconds a presented
    // frame over the last second: all of it, and by kind. 0 while there are no timestamps.
    float gpuMsPerFrame = 0.0f;
    float gpuSetupMs = 0.0f;    // triangle setup and binning
    float gpuShadingMs = 0.0f;  // the shading (the super-samples' work)
    float gpuUploadMs = 0.0f;   // uploads into GS memory, copies, palettes, textures
    float gpuScanoutMs = 0.0f;  // scan-out and read-back
    // Reads of GS memory by the emulator (readTransfer, snapshotVram) over the last second: how many a presented
    // frame, and how long a frame the emulator's thread stood waiting for them (the ring drained to the read,
    // and the graphics card caught up to what it reads). A read waits for everything queued before it, so
    // reads every frame serialise the emulator and the graphics card: that time is the cost.
    float readbacksPerFrame = 0.0f;
    float readbackWaitMs = 0.0f;
    // Of readbackWaitMs, how long a frame the graphics card thread itself stood waiting for the GPU inside
    // paraLLEl-GS (its host reads): the rest of readbackWaitMs is the ring being drained to the read.
    float gpuWaitMs = 0.0f;
    // Submissions the graphics card thread made on its own (eager submits while it had nothing to do), a frame.
    float eagerSubmitsPerFrame = 0.0f;
    // How long a frame the emulator's thread waited for room in the ring (the renderer thread behind).
    float ringWaitMs = 0.0f;
};

// A lock-free triple buffer: one writer (the graphics card thread), one reader (the main thread).
class GSGpuFrameMailbox
{
public:
    GSGpuFrame &writeSlot() noexcept { return m_slot[m_back]; }
    uint32_t writeIndex() const noexcept { return m_back; } // the writer's slot (0-2): never the one the reader holds
    void publish() noexcept
    {
        m_back = m_middle.exchange(m_back | kFresh, std::memory_order_acq_rel) & kIndexMask;
    }
    // The newest published frame if it is newer than lastSeq, else null.
    const GSGpuFrame *take(uint64_t &lastSeq) noexcept
    {
        if ((m_middle.load(std::memory_order_acquire) & kFresh) != 0u)
            m_front = m_middle.exchange(m_front, std::memory_order_acq_rel) & kIndexMask;
        const GSGpuFrame &frame = m_slot[m_front];
        if (frame.seq == 0u || frame.seq == lastSeq)
            return nullptr;
        lastSeq = frame.seq;
        return &frame;
    }

    // The shared image the reader is drawing from (-1: none): the writer never draws into it. Set by the reader
    // before it uses a taken frame's image, cleared once its graphics card work on it is done.
    std::atomic<int> sharedReading{-1};
    // Host-shared pictures (GSGpuOptions::hostSharedFrames): the ring's buffers the reader's graphics card may still be
    // reading (bit n = hostIndex n; set before the reader takes the next frame, cleared once its work on it is done),
    // and the newest generation of which the reader holds nothing any more (no import, no work pending).
    std::atomic<uint32_t> hostReading{0u};
    std::atomic<uint64_t> hostReleasedGeneration{0u};

private:
    static constexpr uint32_t kFresh = 4u;
    static constexpr uint32_t kIndexMask = 3u;
    GSGpuFrame m_slot[3];
    std::atomic<uint32_t> m_middle{1u};
    uint32_t m_back = 0u;  // writer only
    uint32_t m_front = 2u; // reader only
};

class GSGpuRenderer
{
public:
    static constexpr uint32_t kPrivRegCount = 15; // PMODE … BGCOLOR, in GS register order

    virtual ~GSGpuRenderer() = default;

    virtual const char *deviceName() const = 0;

    // The producer (whoever holds the GS front end's state lock), in the front end's order: the raw GS input.
    virtual void gif(uint8_t path, const uint8_t *data, uint32_t bytes) = 0; // whole quadwords
    virtual void reg(uint8_t addr, uint64_t value) = 0;
    virtual void reset() = 0;
    virtual void present(const uint64_t (&privRegs)[kPrivRegCount], uint64_t vsyncTick, bool flip) = 0;
    virtual void kick() = 0;
    // The producer, blocking: answered after everything sent before (GS memory as of this point in the order).
    virtual void snapshotVram(std::vector<uint8_t> &out) = 0;              // all 4 MiB
    virtual uint32_t readTransfer(uint8_t *dst, uint32_t maxBytes) = 0;    // a local -> host transfer's data
    // The producer, blocking: the GS's whole state (registers, the GIF paths, GS memory) as one blob, and back.
    // (Crux2's save states.) False when the blob is not one of this renderer's.
    virtual bool saveState(std::vector<uint8_t> &out) { (void)out; return false; }
    virtual bool loadState(const uint8_t *data, size_t bytes) { (void)data; (void)bytes; return false; }
    // The producer: the GIF paths in `mask` (bit 0 = path 1) start over (a GIF reset).
    virtual void softReset(uint32_t mask) { (void)mask; }

    // The game thread, holding no lock: wait while more than maxAhead presentations are not yet drawn.
    virtual bool waitPresentBacklog(uint32_t maxAhead, const std::atomic<bool> &stop) = 0;
    virtual void wake() = 0;

    // The main thread: the newest finished picture if newer than lastSeq, else null (valid until the next take).
    virtual const GSGpuFrame *takeFrame(uint64_t &lastSeq) = 0;
    // The main thread: the size the picture is shown at (FSR draws at exactly that size).
    virtual void setOutputSize(uint32_t width, uint32_t height) = 0;
    // The main thread, with shared pictures: the shared image it is drawing from (-1 when done with it).
    virtual void setSharedReading(int image) { (void)image; }
    // The main thread, with host-shared pictures: the buffers its graphics card may still read (bit n = hostIndex n),
    // and the newest generation it holds nothing of (see GSGpuFrame::hostPointer).
    virtual void setHostReading(uint32_t mask) { (void)mask; }
    virtual void setHostReleased(uint64_t generation) { (void)generation; }

    // Any thread: live settings, taken at the next presentation.
    virtual void setSuperSampling(uint32_t rate) = 0; // 1, 2, 4, 8 or 16
    virtual void setHighResolution(bool on) = 0;
    virtual void setFsr(bool on) = 0;
    virtual void setSharpness(float stops) = 0;
    virtual void setScanout(const GSGpuScanout &scanout) { (void)scanout; }
    virtual GSGpuStatus status() const = 0;

    // Idempotent; joins the renderer's thread, which owns (and destroys) every Vulkan object.
    virtual void shutdown() = 0;
};

// nullptr when this build has no paraLLEl-GS or the device could not be set up; whyNot says which.
std::unique_ptr<GSGpuRenderer> makeGpuRenderer(const GSGpuOptions &options, std::string &whyNot);
