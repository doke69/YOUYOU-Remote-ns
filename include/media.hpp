#pragma once
#include "protocol.hpp"
#include <SDL.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

struct AVFrame;
namespace uu {
struct Picture {
    int width = 0, height = 0;
    Uint32 pixelFormat = SDL_PIXELFORMAT_RGBA32;
    SDL_YUV_CONVERSION_MODE yuvMode = SDL_YUV_CONVERSION_BT709;
    uint64_t serial = 0;
    // Immutable after publication. The UI keeps a reference instead of copying
    // a 720p RGBA buffer while holding the decode queue mutex every frame.
    std::shared_ptr<const Bytes> pixels;
    std::shared_ptr<const AVFrame> hardwareFrame;
    // Hardware frames carry no CPU buffer but still pin decoder surfaces. Give
    // them a conservative NV12 size so the ready-queue byte bound is not inert.
    size_t pixelBytes() const {
        if (pixels) return pixels->size();
        if (hardwareFrame) return size_t(width > 0 ? width : 0) * size_t(height > 0 ? height : 0) * 3 / 2;
        return 0;
    }
    int64_t mediaTimestamp = INT64_MIN, dueMs = 0, decodedMs = 0, originalDueMs = 0;
    // Fine-grained timing diagnostics for selected-age breakdown:
    int64_t queuedMs = 0;   // When pushPlayout() added this to ready queue
    int64_t selectedMs = 0; // When nextPlayout() selected this for rendering
};
struct MediaStats {
    std::string codec = "—";   // H.264 profile/level parsed from the SPS
    std::string encoder = "—"; // encoder signature parsed from SEI (soft/hard)
    std::string decoder = "—"; // NVDEC hardware / FFmpeg software
    int width = 0, height = 0;
    double fps = 0, decodeMs = 0;
    uint64_t decodeErrors = 0, droppedFrames = 0;
    uint64_t keyframeCount = 0;
    uint64_t decodedTotal = 0, publishedTotal = 0, epoch = 0;
    int inputQueued = 0;
    bool hardwareFrame = false;
    uint64_t cpuTransfers = 0, nativePublished = 0;
    uint64_t skipDue = 0, skipBound = 0;
    int selectedLateMs = 0;
    bool pacing = false;
    int playoutDelayMs = 0, readyFrames = 0;
    uint64_t presentationSkipped = 0, clockResets = 0, missingTimestamps = 0;
    // Presentation selection diagnostics: empty means picture() returned no
    // new frame; future means the next due frame was too far ahead of target.
    uint64_t pictureEmpty = 0, pictureEmptyNoReady = 0, pictureEmptyFuture = 0;
    uint64_t pictureSelected = 0;
    int pictureFutureGapMaxMs = 0;
    uint64_t selectedAgeTotalMs = 0, selectedAgeSamples = 0;
    int selectedAgeMaxMs = 0;
    // Breakdown of selected-age timing
    uint64_t decodeToQueueTotalMs = 0, queueWaitTotalMs = 0;
    uint64_t ageBreakdownSamples = 0;
    // Diagnostic-only access-unit / queue / decode timing. Maxima and >threshold
    // counters are session-cumulative until Media::reset().
    uint64_t auMaxBytes = 0, auBytesTotal = 0, auCount = 0, auOversize = 0;
    uint64_t queueHighFrames = 0, queueHighBytes = 0;
    int queueWaitLastMs = 0, queueWaitMaxMs = 0, oldestQueueWaitMs = 0;
    double decodeMaxMs = 0;
    uint64_t decodeGt8 = 0, decodeGt16 = 0, decodeGt33 = 0;
    int64_t lastDecodedAgeMs = 0;
};
class Media {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Media();
    ~Media();
    void video(const uint8_t *data, size_t size, uint64_t generation,
               std::optional<uint32_t> timestamp = std::nullopt, bool hevc = false,
               std::optional<bool> validatedKeyframe = std::nullopt);
    void audio(const uint8_t *data, size_t size, uint64_t generation);
    uint64_t generation() const;
    Picture picture(uint64_t after, int presentationLeadMs = 0);
    void setPlayoutDelay(int milliseconds);
    void setYuvOutput(bool enabled);
    void setNativeOutput(bool enabled);
    void reset();
    void setVolume(int percent);
    std::string status();
    /// True when the decoder detected lost references (or dropped packets) and
    /// wants the sender to emit a new IDR frame. Clears the pending request.
    bool takeKeyframeRequest();
    /// Milliseconds since the last decoded picture; a paused UI must not
    /// trigger repeated encoder keyframes. Large before the first frame.
    int64_t frameAgeMs();
    /// Decoder / bitstream statistics for the on-screen network panel.
    MediaStats stats();
    /// Number of IDR frames seen so far (used to measure keyframe recovery).
    uint64_t keyframeCount();
};
} // namespace uu
