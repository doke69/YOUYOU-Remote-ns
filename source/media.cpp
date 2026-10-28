#include "cpu_diagnostics.hpp"
#include "media.hpp"
#include "media_integrity.hpp"
#include "playout_policy.hpp"
#include "background.hpp"
#include "config_store.hpp"
#include "diagnostics.hpp"
#include "runtime/latest_snapshot_worker.hpp"
#ifdef __SWITCH__
#include "live_telemetry.hpp"
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#endif
#include <opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}
#include <stdexcept>

namespace uu {
namespace {
// Adapted from upstream v0.1.7: codec resources survive no exceptional exit.
struct DecoderResources {
    AVCodecContext *context=nullptr;
    AVFrame *frame=nullptr,*cpu=nullptr;
    AVPacket *packet=nullptr;
    SwsContext *scaler=nullptr;
    ~DecoderResources() {
        // Destruction order is critical: frames must be freed before their parent context.
        // scaler and packet are independent and can be freed in any order.
        // Frames (cpu, frame) depend on context's memory pool and must be freed first.
        // Context must be freed last to avoid use-after-free in frame cleanup.
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&cpu);   // Free frames before context
        av_frame_free(&frame); // Free frames before context
        avcodec_free_context(&context); // Free context last
    }
};
// Diagnostic-only scope timer: accumulates wall time into a per-access-unit
// nanosecond counter. No waits, fences or locks are added by this helper.
struct StageTimer {
    int64_t &nanos;
    std::chrono::steady_clock::time_point started;
    StageTimer(int64_t &target, std::chrono::steady_clock::time_point start)
        : nanos(target), started(start) {}
    ~StageTimer() {
        nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - started)
                     .count();
    }
};

// EMA (Exponential Moving Average) smoothing constants
constexpr double kSourceFpsEmaAlpha = 0.35;        // Source frame rate estimation weight
constexpr double kDecodeStageEmaAlpha = 0.15;      // Decode stage timing smoothing

int64_t mediaClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void atomicMax(std::atomic<uint64_t> &target, uint64_t value) {
    auto current=target.load(std::memory_order_relaxed);
    while(value>current && !target.compare_exchange_weak(
        current,value,std::memory_order_relaxed,std::memory_order_relaxed)) {}
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}
std::string profileName(int profile) {
    switch (profile) {
    case 66:
        return "Baseline";
    case 77:
        return "Main";
    case 88:
        return "Extended";
    case 100:
        return "High";
    case 110:
        return "High10";
    case 122:
        return "High422";
    case 244:
        return "High444";
    default:
        return "Profile" + std::to_string(profile);
    }
}
// Encoders often announce themselves in an H.264 SEI user_data_unregistered
// message (x264 writes "x264 - core ..."); detect the common ones to tell
// hardware from software encoding on the computer side.
std::string classifyEncoder(const std::string &signature) {
    const auto text = lower(signature);
    if (text.find("x264") != std::string::npos || text.find("lavc") != std::string::npos ||
        text.find("openh264") != std::string::npos)
        return "x264（软编）";
    if (text.find("nvenc") != std::string::npos || text.find("nvidia") != std::string::npos)
        return "NVENC（硬编）";
    if (text.find("qsv") != std::string::npos || text.find("intel") != std::string::npos)
        return "Intel QSV（硬编）";
    if (text.find("amf") != std::string::npos || text.find("amd") != std::string::npos)
        return "AMD AMF（硬编）";
    if (text.find("videotoolbox") != std::string::npos ||
        text.find("apple") != std::string::npos)
        return "VideoToolbox（硬编）";
    if (!signature.empty())
        return signature.substr(0, 32);
    return {};
}
std::string seiSignature(const uint8_t *nal, size_t size) {
    size_t p = 1;
    while (p + 2 <= size) {
        unsigned type = 0;
        while (p < size && nal[p] == 0xFF) {
            type += 255;
            ++p;
        }
        if (p >= size)
            break;
        type += nal[p++];
        unsigned len = 0;
        while (p < size && nal[p] == 0xFF) {
            len += 255;
            ++p;
        }
        if (p >= size)
            break;
        len += nal[p++];
        if (len == 0 || p + len > size)
            break;
        if (type == 5 && len > 16) {
            std::string text;
            for (size_t i = 0; i < std::min<size_t>(len - 16, 96); ++i) {
                const char c = char(nal[p + 16 + i]);
                if (c >= 32 && c < 127)
                    text.push_back(c);
                else if (!text.empty())
                    break;
            }
            if (!text.empty())
                return text;
        }
        p += len;
    }
    return {};
}
std::string trim(std::string value) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}
// Optional decoder override, used to work around driver/decoder issues on a
// specific console or firmware: UU_DECODER=software|hardware|auto, or
// sdmc:/switch/UURemote-ns/config.ini with a "decoder=..." line.
std::string decoderPreference() {
    if (const char *env = std::getenv("UU_DECODER"); env && *env)
        return trim(env);
    const auto configured = ConfigStore::instance().value("decoder");
    return configured.empty() ? std::string("auto") : configured;
}
bool prefersHardware(const std::string &preference) {
    std::string value = preference;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return value != "software" && value != "sw" && value != "off" && value != "0";
}
bool diagEnabled() {
    if (const char *env = std::getenv("UU_DIAG"); env && *env)
        return std::string(env) != "0" && std::string(env) != "off";
    const auto value = ConfigStore::instance().value("diag");
    return !value.empty() && value != "0" && value != "off" && value != "no";
}
bool strictReceiverEnabled() {
    return ConfigStore::instance().value("receiver") != "legacy";
}
int mediaConfigInt(const std::string &key, int fallback, int low, int high) {
    return ConfigStore::instance().integer(key,fallback,low,high);
}
const char *diagDirectory() {
#ifdef __SWITCH__
    return "sdmc:/switch/UURemote-ns/diag";
#else
    return "diag";
#endif
}
} // namespace

struct Media::Impl {
    std::mutex mutex, audioMutex;
    std::condition_variable cv;
    struct PacketFree { void operator()(AVPacket *value) const noexcept { av_packet_free(&value); } };
    using PacketRef = std::unique_ptr<AVPacket, PacketFree>;
    struct EncodedUnit {
        PacketRef packet;
        size_t byteSize=0;
        int64_t timestamp=AV_NOPTS_VALUE;
        bool hevc=false;
        int64_t enqueuedMs=0;
        std::optional<bool> validatedKeyframe;
    };
    std::deque<EncodedUnit> queue;
    size_t queueBytes = 0, queueHighFrames = 0, queueHighBytes = 0;
    int64_t queueWaitLastMs = 0, queueWaitMaxMs = 0;
    std::atomic<uint64_t> auBytesTotal{0}, auMaxBytes{0}, auOversize{0}, auCountDiag{0};
    std::atomic<uint64_t> decodeMaxUs{0}, decodeGt8{0}, decodeGt16{0}, decodeGt33{0};
    Picture latest;
    std::deque<Picture> ready;
    size_t readyBytes = 0;
    // Up to 100 ms of requested delay at 60 fps plus two burst frames.
    // Hardware frames still count against the independent 32 MiB bound.
    static constexpr size_t kMaxReadyFrames = 8;
    int streamFps = 60;
    int64_t arrivalWindowStartMs = 0, lastArrivalMs = 0;
    size_t arrivalWindowFrames = 0;
    double estimatedSourceFps = 0.0;
    int64_t lastAgeBreakdownLogMs = 0;
    uint64_t playoutOverflowDrop = 0, playoutEmptyRepeat = 0, playoutHold = 0;
    bool pacing = mediaConfigInt("frame_pacing",1,0,1)!=0;
    int playoutDelayMs = mediaConfigInt("playout_delay_ms",35,0,100);
    bool haveInputTimestamp = false;
    int64_t highestInputTimestamp = 0;
    int64_t lastPresentedTimestamp = AV_NOPTS_VALUE;
    uint64_t presentationSkipped = 0, clockResets = 0, missingTimestamps = 0;
    uint64_t pictureEmpty = 0, pictureEmptyNoReady = 0, pictureEmptyFuture = 0;
    uint64_t pictureSelected = 0;
    int pictureFutureGapMaxMs = 0;
    uint64_t selectedAgeTotalMs = 0, selectedAgeSamples = 0;
    int selectedAgeMaxMs = 0;
    uint64_t decodeToQueueTotalMs = 0, queueWaitTotalMs = 0;
    uint64_t ageBreakdownSamples = 0;
    uint64_t skipDue = 0, skipBound = 0;
    int selectedLateMs = 0;
    std::deque<std::string> playoutTrace;
    void tracePicture(const Picture &picture, int64_t now, const char *reason, int64_t targetMs = 0) {
        // Bounded numeric-only trace, independent of the AU/video dump switch.
        // 6000 rows (~60-100 s at 60 fps) is still a few hundred KiB and makes
        // the presentation-drop trigger analyzable after a reset.
        if(playoutTrace.size()>=6000) playoutTrace.pop_front();
        playoutTrace.push_back(std::to_string(picture.mediaTimestamp)+","+std::to_string(picture.decodedMs)+","+
            std::to_string(picture.dueMs)+","+std::to_string(now)+","+reason+","+std::to_string(targetMs?targetMs:now)+","+std::to_string(picture.originalDueMs));
    }
    void resetPlayout() {
        ready.clear();readyBytes=0;latest={};
        arrivalWindowStartMs=0;lastArrivalMs=0;arrivalWindowFrames=0;
        estimatedSourceFps=0;lastAgeBreakdownLogMs=0;
    }
    void noteArrival(int64_t now) {
        if(!lastArrivalMs || now-lastArrivalMs>500) {
            arrivalWindowStartMs=now;lastArrivalMs=now;arrivalWindowFrames=1;return;
        }
        lastArrivalMs=now;++arrivalWindowFrames;
        if(now-arrivalWindowStartMs<250 || arrivalWindowFrames<2)return;
        double fps=double(arrivalWindowFrames-1)*1000.0/double(now-arrivalWindowStartMs);
        fps=std::clamp(fps,1.0,double(std::max(30,streamFps)));
        estimatedSourceFps=estimatedSourceFps<=0?fps:estimatedSourceFps*(1.0-kSourceFpsEmaAlpha)+fps*kSourceFpsEmaAlpha;
        arrivalWindowStartMs=now;arrivalWindowFrames=1;
    }
    void trimPlayout(size_t keep) {
        while(ready.size()>keep) {
            tracePicture(ready.front(),mediaClockMs(),"queue_bound");++skipBound;
            readyBytes-=ready.front().pixelBytes();ready.pop_front();++presentationSkipped;++playoutOverflowDrop;
        }
    }
    void pushPlayout(Picture p) {
        const auto now = mediaClockMs();
        p.queuedMs = now;
        // This is an output hold from decode completion, not a network-jitter
        // guarantee. A late publication is immediately eligible, not delayed again.
        p.dueMs = p.decodedMs + playoutDelayMs;
        p.originalDueMs = p.dueMs;
        readyBytes+=p.pixelBytes();
        ready.push_back(std::move(p));
        noteArrival(now);
        trimPlayout(kMaxReadyFrames);
        while(readyBytes>32u*1024u*1024u && !ready.empty()) {
            tracePicture(ready.front(),mediaClockMs(),"queue_bytes");++skipBound;
            readyBytes-=ready.front().pixelBytes();ready.pop_front();++presentationSkipped;++playoutOverflowDrop;
        }
    }
    Picture nextPlayout(uint64_t after, int64_t now) {
        // An import that failed transiently may retry its not-yet-consumed
        // selected frame. Already-consumed frames stay in the renderer instead.
        if(latest.serial>after)return latest;
        while(!ready.empty() && ready.front().serial<=after) {
            tracePicture(ready.front(),now,"stale_serial");
            readyBytes-=ready.front().pixelBytes();ready.pop_front();++presentationSkipped;
        }
        if(ready.empty()) {
            ++pictureEmpty;++pictureEmptyNoReady;++playoutEmptyRepeat;
            return {};
        }
        // Elapsed-time deadlines replace per-poll source-rate credit. Source
        // rate only sets the bounded recovery allowance, never creates holds.
        const double sourceFps=lastArrivalMs && now-lastArrivalMs<=500 && estimatedSourceFps>0
            ? estimatedSourceFps : double(std::max(1,streamFps));
        // Include one display tick so ordinary phase/jitter does not trigger
        // a discard just before the next refresh. This is recovery tolerance,
        // not an added delay: eligible frames are still selected immediately.
        const int64_t lateBudget=17+int64_t(std::ceil(1000.0/sourceFps));
        const auto decision=choosePlayout(ready,now,lateBudget);
        if(!decision.available) {
            ++pictureEmpty;++pictureEmptyFuture;++playoutHold;
            pictureFutureGapMaxMs=std::max(pictureFutureGapMaxMs,
                int(std::min<int64_t>(1000,ready.front().dueMs-now)));
            return {};
        }
        const size_t consume=decision.discard+1;
        Picture selected;
        for(size_t i=0;i<consume;++i) {
            selected=std::move(ready.front());ready.pop_front();
            readyBytes-=selected.pixelBytes();
            if(i+1<consume) {++presentationSkipped;++skipDue;}
        }
        latest=std::move(selected);
        latest.selectedMs = now;
        selectedLateMs=int(std::max<int64_t>(0,now-latest.dueMs));
        ++pictureSelected;
        const int64_t age=now-latest.decodedMs;
        if(age>=0) {++selectedAgeSamples;selectedAgeTotalMs+=uint64_t(age);
            selectedAgeMaxMs=std::max(selectedAgeMaxMs,int(age));}

        // Fine-grained age breakdown diagnostic and stats collection
        const int64_t decodeToQueue = latest.queuedMs > 0 ? latest.queuedMs - latest.decodedMs : -1;
        const int64_t queueWait = latest.queuedMs > 0 ? now - latest.queuedMs : -1;
        if (decodeToQueue >= 0 && queueWait >= 0) {
            ++ageBreakdownSamples;
            decodeToQueueTotalMs += uint64_t(decodeToQueue);
            queueWaitTotalMs += uint64_t(queueWait);
            // Fixed-rate summaries carry the full distribution. Throttle
            // formatted single-frame events while the media mutex is held.
            if (age > 40 && (!lastAgeBreakdownLogMs || now-lastAgeBreakdownLogMs>=1000)) {
                lastAgeBreakdownLogMs=now;
                diagnostic(("selected-age-breakdown-total-" + std::to_string(age) +
                           "ms-decode-to-queue-" + std::to_string(decodeToQueue) +
                           "ms-queue-wait-" + std::to_string(queueWait) + "ms").c_str());
            }
        }

        tracePicture(latest,now,"selected",now);
        lastPresentedTimestamp=latest.mediaTimestamp;lastFrameMs=now;
        return latest;
    }

    std::atomic<bool> yuvOutput{false}, nativeOutput{false};
    std::atomic<uint64_t> cpuTransfers{0},nativePublished{0};
    bool stop = false, clear = false;
    std::thread thread;
    SDL_AudioDeviceID audioDevice = 0;
    std::atomic<int> volumePercent{100};
    OpusDecoder *opus = nullptr;
    std::string error;
    uint64_t serial = 0;
    std::atomic<uint64_t> generation{1};
    // Incremented on same-generation recovery (input overflow) and reset;
    // captured by the codec worker when it dequeues an AU.
    std::atomic<uint64_t> recovery{0};
    std::atomic<bool> keyframeRequested{true};
    std::atomic<bool> hardware{false};
    std::atomic<int64_t> lastFrameMs{0};
    std::atomic<int64_t> lastDecodedMs{0};
    std::atomic<uint64_t> decodeErrors{0}, droppedUnits{0};
    std::atomic<int> width{0}, height{0};
    std::atomic<double> fps{0}, decodeMs{0};
    std::atomic<uint64_t> keyframeCount{0};
    std::atomic<uint64_t> decodedTotal{0}, publishedTotal{0};
    std::atomic<bool> hardwareFrame{false};
    std::string codecText = "—", encoderText = "—";
    uint64_t cleanFrames = 0;
    uint64_t fpsFrames = 0;
    int64_t fpsWindow = 0;
    bool diag_ = diagEnabled();
    bool strictReceiver_ = strictReceiverEnabled();
    std::vector<uint8_t> auDump_;
    size_t auLimit_ = 32u * 1024u * 1024u;
    uint64_t auCount_ = 0;
    Picture diagnosticPicture_;
    bool diagnosticHevc_ = false;
    bool hintedCodecPublished_ = false;
    std::atomic<bool> decoderFailed{false};
    enum class AudioStatus { Ready, Unavailable, QueueFailed };
    std::atomic<AudioStatus> audioStatus{AudioStatus::Ready};
    // RTP timestamps share the video playout delay. Held packets stay ordered
    // until that deadline; only a short gap is concealed.
    std::map<int64_t, std::vector<uint8_t>> audioHold;
    bool audioAnchored=false, audioHaveClock=false, audioHavePlay=false;
    int64_t audioAnchorRtp=0, audioAnchorDueMs=0, audioExtendedRtp=0, audioPlayRtp=0;
    uint32_t audioLastRtp32=0;
    int audioDelayMs=0, audioFrameSamples=960;
    std::array<int16_t,5760*2> audioPcm{};
    void resetAudioSchedule();
    void queueAudioPcm(int samples);
    void concealAudioUntil(int64_t rtp);
    void releaseAudio();
    void ingestAudio(const uint8_t *data, size_t size, std::optional<uint32_t> timestamp);
    Impl() {
      try {
        audioDelayMs=playoutDelayMs;
        SDL_AudioSpec spec{};
        spec.freq = 48000;
        spec.format = AUDIO_S16SYS;
        spec.channels = 2;
        spec.samples = 960;
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
        if(!audioDevice) audioStatus=AudioStatus::Unavailable;
        int code = 0;
        opus = opus_decoder_create(48000, 2, &code);
        if (code != OPUS_OK)
            throw std::runtime_error("Opus decoder initialization failed");
        if (audioDevice)
            SDL_PauseAudioDevice(audioDevice, 0);
        thread = std::thread([this] {
            try {decode();}
            catch(...) {decoderFailed=true;diagnosticException("video-decode-exception",std::current_exception());}
        });
      } catch(...) {
        if(audioDevice) SDL_CloseAudioDevice(audioDevice);
        opus_decoder_destroy(opus);throw;
      }
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        cv.notify_one();
        thread.join();
        if (audioDevice)
            SDL_CloseAudioDevice(audioDevice);
        opus_decoder_destroy(opus);
        flushDiagnostics();
    }
    struct DiagnosticDump {
        std::deque<std::string> playoutTrace;
        std::vector<uint8_t> auDump_;
        Picture diagnosticPicture_;
        std::string codecText, encoderText;
        bool diag_=false, diagnosticHevc_=false, hardware=false, pacing=false;
        int width=0, height=0, playoutDelayMs=0;
        size_t readyFrames=0;
        uint64_t auCount_=0, decodeErrors=0, droppedUnits=0, keyframeCount=0;
        uint64_t presentationSkipped=0, clockResets=0, missingTimestamps=0, order=0;
        double fps=0, decodeMs=0;
    };
    struct DiagnosticJob {
        uint64_t order;
        size_t bytes;
        std::unique_ptr<DiagnosticDump> dump;
        size_t retainedBytes() const noexcept { return bytes; }
    };
    std::mutex diagnosticSubmitMutex;
    std::unique_ptr<runtime::LatestSnapshotWorker<DiagnosticJob>> diagnosticWriter;
    std::atomic<bool> diagnosticWriterUnavailable{false};
    void queueDiagnostics(DiagnosticDump &&dump) noexcept {
        if(dump.playoutTrace.empty() && dump.auDump_.empty()) return;
        // Only CPU pixels are written. Never retain a hardware surface for a
        // diagnostic that cannot use it, especially across slow storage writes.
        dump.diagnosticPicture_.hardwareFrame.reset();
        size_t bytes=dump.auDump_.capacity();
        for(const auto &line:dump.playoutTrace) bytes+=line.capacity();
        if(dump.diagnosticPicture_.pixels) bytes+=dump.diagnosticPicture_.pixels->capacity();
        try {
            std::lock_guard lock(diagnosticSubmitMutex);
            if(diagnosticWriterUnavailable) return;
            if(!diagnosticWriter)
                diagnosticWriter=std::make_unique<runtime::LatestSnapshotWorker<DiagnosticJob>>(
                    96u*1024u*1024u,[](const DiagnosticJob &job){writeDiagnostics(*job.dump);});
            const auto order=dump.order;
            if(!diagnosticWriter->submit({order,bytes,std::make_unique<DiagnosticDump>(std::move(dump))}))
                diagnostic("media-diagnostics-snapshot-rejected");
        } catch(...) {
            diagnosticWriterUnavailable=true;
            diagnosticException("media-diagnostics-worker-unavailable",std::current_exception());
        }
    }
    // Caller owns mutex, or has joined the decoder during destruction.
    DiagnosticDump takeDiagnostics() {
        DiagnosticDump d;
        static std::atomic<uint64_t> sequence{0};
        d.order=++sequence;
        d.playoutTrace.swap(playoutTrace); d.auDump_.swap(auDump_);
        d.diagnosticPicture_=std::move(diagnosticPicture_);
        d.diag_=diag_;
        d.diagnosticHevc_=diagnosticHevc_;
        d.auCount_=auCount_;
        d.hardware=hardware.load();
        d.codecText=codecText;
        d.encoderText=encoderText;
        d.width=width.load();
        d.height=height.load();
        d.decodeErrors=decodeErrors.load();
        d.droppedUnits=droppedUnits.load();
        d.keyframeCount=keyframeCount.load();
        d.fps=fps.load();
        d.decodeMs=decodeMs.load();
        d.pacing=pacing;
        d.playoutDelayMs=playoutDelayMs;
        d.presentationSkipped=presentationSkipped;
        d.clockResets=clockResets;
        d.missingTimestamps=missingTimestamps;
        d.readyFrames=ready.size();
        return d;
    }
    static void writeDiagnostics(const DiagnosticDump &d) {
        const bool haveTiming = !d.playoutTrace.empty();
        const bool haveMediaDump = d.diag_ && !d.auDump_.empty();
        if (!haveTiming && !haveMediaDump) return;
        // Jobs can finish out of order across reconnects. An older snapshot
        // must never overwrite files already written for a newer session.
        static std::mutex outputMutex;
        static uint64_t written=0;
        std::lock_guard lock(outputMutex);
        if(d.order<written) return;
        written=d.order;

#ifdef _WIN32
        _mkdir(diagDirectory());
#else
        mkdir(diagDirectory(), 0777);
#endif
        // Numeric timing is independent of AU/video dump. It contains only
        // timestamps/selection reasons, never pixels, endpoints or accounts.
        if (haveTiming) {
            std::ofstream timing(std::string(diagDirectory()) + "/playout_frames.csv");
            timing << "media_timestamp_90khz,decoded_ms,due_ms,selected_ms,result,target_present_ms,raw_due_ms\n";
            for(const auto &row:d.playoutTrace) timing << row << '\n';
        }
        if (!haveMediaDump)
            return;
        std::ofstream file(std::string(diagDirectory()) + (d.diagnosticHevc_?"/video.h265":"/video.h264"), std::ios::binary);
        if (file)
            file.write(reinterpret_cast<const char *>(d.auDump_.data()), std::streamsize(d.auDump_.size()));
        std::ofstream info(std::string(diagDirectory()) + "/media_info.txt");
        // Snapshot after conversion and before SDL upload: distinguishes the
        // decoded image from a renderer/texture stride or synchronisation issue.
        if (d.diagnosticPicture_.pixels) {
            const auto &p = d.diagnosticPicture_;
            std::ofstream ppm(std::string(diagDirectory()) + "/decoded_before_display.ppm", std::ios::binary);
            if (ppm) {
                // Diagnostics convert one retained frame, never each live frame.
                Bytes rgb(size_t(p.width)*p.height*3);
                const bool nv12=p.pixelFormat==SDL_PIXELFORMAT_NV12;
                SwsContext *snapshot=sws_getContext(p.width,p.height,nv12?AV_PIX_FMT_NV12:AV_PIX_FMT_RGBA,
                    p.width,p.height,AV_PIX_FMT_RGB24,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
                if(snapshot) {
                    const int *coeff=sws_getCoefficients(p.yuvMode==SDL_YUV_CONVERSION_BT709?SWS_CS_ITU709:SWS_CS_ITU601);
                    if(nv12) sws_setColorspaceDetails(snapshot,coeff,0,coeff,1,0,1<<16,1<<16);
                    const uint8_t *src[]={p.pixels->data(),nv12?p.pixels->data()+size_t(p.width)*p.height:nullptr,nullptr,nullptr};
                    int pitch[]={nv12?p.width:p.width*4,nv12?p.width:0,0,0};
                    uint8_t *dst[]={rgb.data()};int stride[]={p.width*3};
                    if(sws_scale(snapshot,src,pitch,0,p.height,dst,stride)==p.height) {
                        ppm << "P6\n" << p.width << ' ' << p.height << "\n255\n";
                        ppm.write(reinterpret_cast<const char *>(rgb.data()),std::streamsize(rgb.size()));
                    }
                    sws_freeContext(snapshot);
                }
            }
        }
        if (info) {
            info << "aus=" << d.auCount_ << "\n";
            info << "dump_bytes=" << d.auDump_.size() << "\n";
            info << "decoder=" << (d.hardware ? "nvdec" : "software") << "\n";
            info << "codec=" << d.codecText << "\n";
            info << "encoder=" << d.encoderText << "\n";
            info << "width=" << d.width << "\n";
            info << "height=" << d.height << "\n";
            info << "decode_errors=" << d.decodeErrors << "\n";
            info << "dropped=" << d.droppedUnits << "\n";
            info << "keyframes=" << d.keyframeCount << "\n";
            info << "fps=" << d.fps << "\n";
            info << "decode_ms=" << d.decodeMs << "\n";
            info << "frame_pacing=" << d.pacing << "\nplayout_delay_ms=" << d.playoutDelayMs
                 << "\nready_frames=" << d.readyFrames << "\npresentation_skipped=" << d.presentationSkipped
                 << "\nplayout_clock_resets=" << d.clockResets << "\nmissing_frame_timestamps=" << d.missingTimestamps << '\n';
            // Legacy diagnostic keys are intentionally inactive and fixed at 0.
            info << "cadence_smoothing_ms=0\nsmoothed_frames=0\nmax_due_shift_ms=0\n";
        }
    }
    void flushDiagnostics() {
        queueDiagnostics(takeDiagnostics());
        if(diagnosticWriter) {
            // Final storage drain is intentionally joined, not a hard timeout.
            diagnosticWriter->shutdown();
            const auto stats=diagnosticWriter->stats();
            char text[192];
            std::snprintf(text,sizeof text,"media-diagnostics-completed-%llu-replaced-%llu-rejected-%llu-failures-%llu-peak-bytes-%llu",
                (unsigned long long)stats.completed,(unsigned long long)stats.replaced,
                (unsigned long long)stats.rejected,(unsigned long long)stats.failures,
                (unsigned long long)stats.peakBytes);
            diagnostic(text);
        }
    }
    static AVPixelFormat chooseFormat(AVCodecContext *context, const AVPixelFormat *formats) {
#ifdef __SWITCH__
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (context->hw_device_ctx && *p == AV_PIX_FMT_NVTEGRA)
                return *p;
#else
        (void)context;
#endif
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_NV12)
                return *p;
        return formats[0];
    }
    // Parse the Annex-B access unit for SPS/SEI/IDR so the network panel can
    // show profile/level, the encoder signature and count keyframes.
    void inspect(const uint8_t *data, size_t length, bool hevc, std::optional<bool> validatedKeyframe, uint64_t epoch) {
        if (length < 5) return;
        if (hevc && validatedKeyframe.has_value()) {
            // The strict assembler already parsed every NAL and proved this AU.
            // This only avoids a second scan for UI diagnostics; FFmpeg still
            // parses/validates the encoded packet normally.
            std::lock_guard lock(mutex);
            if(epoch!=generation.load()) return;
            diagnosticHevc_=hevc;
            if (!hintedCodecPublished_) {
                codecText="H.265 Main";hintedCodecPublished_=true;
            }
            // Preserve requested compressed dumps without rescanning their NALs.
            if (diag_) {
                ++auCount_;
                if (auDump_.size()+length<=auLimit_)
                    auDump_.insert(auDump_.end(),data,data+length);
            }
            if (*validatedKeyframe) ++keyframeCount;
            return;
        }
        std::string codec, encoder;
        bool idr = false;
        size_t pos = 0;
        while (pos + 3 < length) {
            size_t start = size_t(-1), startLength = 0;
            for (size_t i = pos; i + 2 < length; ++i) {
                if (data[i] != 0 || data[i + 1] != 0)
                    continue;
                if (data[i + 2] == 1) {
                    start = i;
                    startLength = 3;
                    break;
                }
                if (i + 3 < length && data[i + 2] == 0 && data[i + 3] == 1) {
                    start = i;
                    startLength = 4;
                    break;
                }
            }
            if (start == size_t(-1))
                break;
            const size_t begin = start + startLength;
            size_t end = length;
            for (size_t i = begin + 1; i + 2 < length; ++i) {
                if (data[i] != 0 || data[i + 1] != 0)
                    continue;
                if (data[i + 2] == 1 || (i + 3 < length && data[i + 2] == 0 && data[i + 3] == 1)) {
                    end = i;
                    break;
                }
            }
            if (begin >= end) {
                pos = end;
                continue;
            }
            const uint8_t *nal = data + begin;
            const size_t size = end - begin;
            const int type = hevc?(nal[0]>>1)&63:nal[0]&31;
            if(hevc) {
                codec="H.265 Main";
                if(type>=16 && type<=20) idr=true;
            } else if (type == 5) {
                idr = true;
            } else if (type == 7 && size >= 4) {
                codec = "H.264 " + profileName(nal[1]) + "@" + std::to_string(nal[3] / 10) + "." +
                        std::to_string(nal[3] % 10);
            } else if (type == 6) {
                const auto signature = seiSignature(nal, size);
                if (!signature.empty()) {
                    const auto classified = classifyEncoder(signature);
                    if (!classified.empty())
                        encoder = classified;
                }
            }
            if (end == length)
                break;
            pos = end;
        }
        std::lock_guard lock(mutex);
        if(epoch!=generation.load()) return;
        diagnosticHevc_=hevc;
        if(!hevc) hintedCodecPublished_=false;
        if (diag_) {
            ++auCount_;
            if (auDump_.size() + length <= auLimit_)
                auDump_.insert(auDump_.end(), data, data + length);
        }
        if (idr)
            ++keyframeCount;
        if (!codec.empty() || !encoder.empty()) {
            if (!codec.empty() && codecText != codec)
                codecText = codec;
            if (!encoder.empty() && encoderText != encoder)
                encoderText = encoder;
        }
    }
    bool openDecoder(AVCodecContext *&ctx, AVCodecID id) {
        const auto *codec=avcodec_find_decoder(id);
        if(!codec) return false;
            bool opened = false;
            auto open = [&](bool hw) {
                avcodec_free_context(&ctx);
                ctx = avcodec_alloc_context3(codec);
                if (!ctx) {
                    std::lock_guard lock(mutex);
                    error = "视频解码器内存不足";
                    return false;
                }
                // Hardware decoding is single-threaded; software decoding uses
                // frame threading to keep 1080p H.264 decodable on the A57s.
                ctx->thread_count = hw ? 1 : 3;
                ctx->pkt_timebase = AVRational{1,90000};
                ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
#ifdef __SWITCH__
                ctx->get_format = chooseFormat;
                if (hw && av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA, nullptr,
                                                 nullptr, 0) < 0) {
                    std::lock_guard lock(mutex);
                    error = "NVDEC 初始化失败，改用软件解码";
                    return false;
                }
#else
                (void)hw;
#endif
                if (avcodec_open2(ctx, codec, nullptr) < 0) {
                    std::lock_guard lock(mutex);
                    error = hw ? "NVDEC 初始化失败，改用软件解码" : "无法初始化视频解码器";
                    return false;
                }
                hardware = hw;
                return true;
            };
#ifdef __SWITCH__
            if (prefersHardware(decoderPreference()))
                opened = open(true);
#else
            (void)prefersHardware;
#endif
            if (!opened)
                opened = open(false);
            return opened;
    }
    void decode() {
        uu_cpu_mark(1);
        DecoderResources resources;
        AVCodecID activeCodec=AV_CODEC_ID_H264;
        auto *&frame=resources.frame;frame=av_frame_alloc();
        auto *&cpu=resources.cpu;cpu=av_frame_alloc();
        auto *&packet=resources.packet;packet=av_packet_alloc();
        auto *&sws=resources.scaler;
        auto *&ctx=resources.context;
        // Diagnostic stage EMAs (microseconds): send, receive/drain, frame
        // processing + publish. Existing decodeMs semantics are unchanged.
        double stageSendUs=0, stageRecvUs=0, stagePublishUs=0;
        bool receiveEofLogged=false;
        auto ema=[](double value,double sample){return value<=0?sample:value+(sample-value)*kDecodeStageEmaAlpha;};
        // Codec state belongs only to this worker. Generation checks protect
        // publication; reset never touches a live codec or waits for its fence.
        uint64_t stageEpoch=generation.load();
        if (!frame || !cpu || !packet) {
            std::lock_guard lock(mutex);
            error = "视频解码器内存不足";
            decoderFailed=true;
            goto done;
        }
        if(!openDecoder(ctx,activeCodec)) {decoderFailed=true;goto done;}
        while (true) {
            EncodedUnit unit;
            uint64_t currentGeneration;
            uint64_t currentRecovery;
            bool flushCodec=false;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop || clear || !queue.empty(); });
                if (stop)
                    break;
                if (clear) { flushCodec=true; clear=false; }
                if(flushCodec) {
                    // Driver/fence waits must not block picture() or reset().
                    lock.unlock();
                    avcodec_flush_buffers(ctx);
                    receiveEofLogged=false;
                    lock.lock();
                    // Recheck stop/clear and generation after the unlocked wait.
                    continue;
                }
                if (queue.empty()) continue;
                unit = std::move(queue.front());
                queue.pop_front();
                if(unit.byteSize<=queueBytes) queueBytes-=unit.byteSize; else queueBytes=0;
                const auto dequeuedMs=mediaClockMs();
                queueWaitLastMs=unit.enqueuedMs?std::max<int64_t>(0,dequeuedMs-unit.enqueuedMs):0;
                queueWaitMaxMs=std::max(queueWaitMaxMs,queueWaitLastMs);
                currentGeneration = generation.load();
                currentRecovery = recovery.load();
            }
            const uint64_t activeGeneration=generation.load();
            if(activeGeneration!=stageEpoch) {
                // Never blend local EMA values across session generations.
                stageSendUs=stageRecvUs=stagePublishUs=0;
                stageEpoch=activeGeneration;
            }
            if (currentGeneration != activeGeneration)
                continue;
            if (currentRecovery != recovery.load())
                continue;
            const auto wanted=unit.hevc?AV_CODEC_ID_HEVC:AV_CODEC_ID_H264;
            if(wanted!=activeCodec) {
                av_frame_unref(frame);av_frame_unref(cpu);av_packet_unref(packet);
                sws_freeContext(sws);sws=nullptr;
                if(!openDecoder(ctx,wanted)) {decoderFailed=true;goto done;}
                activeCodec=wanted;
                receiveEofLogged=false;
                std::lock_guard lock(mutex);
                if(!mediaPublishAllowed(currentGeneration,generation.load(),
                                        currentRecovery,recovery.load())) continue;
                presentationSkipped+=ready.size();resetPlayout();
                codecText=unit.hevc?"H.265 Main":"H.264";
            }
            uu_cpu_mark(1);
            av_packet_unref(packet);
            av_packet_move_ref(packet,unit.packet.get());
            inspect(packet->data,size_t(packet->size),unit.hevc,unit.validatedKeyframe,currentGeneration);
            packet->pts=unit.timestamp;
            packet->dts=AV_NOPTS_VALUE;
            // Nanosecond stage accumulators for this access unit:
            // sendNs = avcodec_send_packet wall time (both EAGAIN attempts);
            // recvNs = every avcodec_receive_frame call, including EAGAIN/error
            // returns (its blocking time includes NVTEGRA post-process wait);
            // publishNs = from a successful receive return through frame
            // handling, queue publication and av_frame_unref (RAII timer).
            int64_t sendNs=0, recvNs=0, publishNs=0;
            auto drain = [&] {
                while (true) {
                    const auto receiveStarted=std::chrono::steady_clock::now();
                    const int receiveResult=avcodec_receive_frame(ctx,frame);
                    const auto frameReceived=std::chrono::steady_clock::now();
                    recvNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
                               frameReceived-receiveStarted).count();
                    const auto disposition=classifyReceiveResult(receiveResult,AVERROR(EAGAIN),AVERROR_EOF);
                    if(disposition==ReceiveDisposition::Again) break;
                    if(disposition==ReceiveDisposition::Eof) {
                        if(!receiveEofLogged) {
                            char message[96];
                            std::snprintf(message,sizeof message,"video-receive-eof-generation-%llu",
                                static_cast<unsigned long long>(currentGeneration));
                            diagnostic(message);
                            receiveEofLogged=true;
                        }
                        break;
                    }
                    if(disposition==ReceiveDisposition::Error) {
                        const bool current=mediaPublishAllowed(currentGeneration,generation.load(),
                                                               currentRecovery,recovery.load());
                        if(current) {
                            {
                                std::lock_guard lock(mutex);
                                if(mediaPublishAllowed(currentGeneration,generation.load(),
                                                       currentRecovery,recovery.load())) {
                                    ++decodeErrors;
                                    keyframeRequested=true;
                                }
                            }
                            char message[128];
                            std::snprintf(message,sizeof message,
                                "video-receive-error-code-%d-generation-%llu-recovery-%llu",
                                receiveResult,
                                static_cast<unsigned long long>(currentGeneration),
                                static_cast<unsigned long long>(currentRecovery));
                            diagnostic(message);
                        }
                        break;
                    }
                    receiveEofLogged=false;
                    StageTimer publishTimer(publishNs,frameReceived);
                    bool validEpoch=false;
                    {
                        std::lock_guard lock(mutex);
                        validEpoch=mediaPublishAllowed(currentGeneration,generation.load(),
                                                       currentRecovery,recovery.load());
                        if(validEpoch) {
                            ++decodedTotal;
#ifdef __SWITCH__
                            hardwareFrame=frame->format==AV_PIX_FMT_NVTEGRA;
#else
                            hardwareFrame=false;
#endif
                        }
                    }
                    if(!validEpoch) { av_frame_unref(frame); continue; }
                    const bool damaged = frame->decode_error_flags != 0;
                    if (damaged) {
                        std::lock_guard lock(mutex);
                        if(mediaPublishAllowed(currentGeneration,generation.load(),
                                               currentRecovery,recovery.load())) {
                            ++decodeErrors;
                            keyframeRequested = true;
                        }
                    }
                    AVFrame *source = frame;
#ifdef __SWITCH__
                    if (frame->format == AV_PIX_FMT_NVTEGRA && !nativeOutput.load()) {
                        av_frame_unref(cpu);
                        {
                            std::lock_guard lock(mutex);
                            if(mediaPublishAllowed(currentGeneration,generation.load(),
                                                   currentRecovery,recovery.load())) ++cpuTransfers;
                        }
                        if (av_hwframe_transfer_data(cpu, frame, 0) < 0) {
                            {
                                std::lock_guard lock(mutex);
                                if(mediaPublishAllowed(currentGeneration,generation.load(),
                                                       currentRecovery,recovery.load())) {
                                    ++decodeErrors;
                                    keyframeRequested = true;
                                }
                            }
                            av_frame_unref(frame);
                            continue;
                        }
                        source = cpu;
                    }
#endif
                    if (source->width <= 0 || source->height <= 0 || source->width > 4096 ||
                        source->height > 2160) {
                        av_frame_unref(frame);
                        continue;
                    }
                    // A frame that lost its references is garbage until the next
                    // IDR. Keep showing the last good frame while a keyframe is
                    // requested; fall back to showing damage if nothing clean
                    // arrived for a while so the picture does not freeze forever.
                    const auto now = mediaClockMs();
                    bool show=false;
                    {
                        std::lock_guard lock(mutex);
                        show=mediaPublishAllowed(currentGeneration,generation.load(),
                                                  currentRecovery,recovery.load()) &&
                            (!damaged || (!strictReceiver_ && (cleanFrames==0 || now-lastFrameMs.load()>2000)));
                    }
                    if (show) {
                        Picture p;
                        p.mediaTimestamp=frame->best_effort_timestamp;
                        if(p.mediaTimestamp==AV_NOPTS_VALUE) p.mediaTimestamp=frame->pts;
                        p.decodedMs=now;
                        p.width = source->width;
                        p.height = source->height;
                        bool converted=false;
#ifdef __SWITCH__
                        if(nativeOutput.load() && frame->format==AV_PIX_FMT_NVTEGRA) {
                            // receive_frame has run NVTEGRA's post_process wait;
                            // retain the original surface until the display fence.
                            auto *reference=av_frame_clone(frame);
                            if(reference) {
                                p.hardwareFrame=std::shared_ptr<const AVFrame>(reference,[](const AVFrame *value) {
                                    auto *mutableFrame=const_cast<AVFrame *>(value);av_frame_free(&mutableFrame);
                                });
                                p.pixelFormat=SDL_PIXELFORMAT_NV12;converted=true;
                            }
                        } else
#endif
                        {
                            // Keep 8-bit limited-range 4:2:0 on the CPU side. The
                            // native SDL shader performs colour conversion. This
                            // still transfers NVDEC frames to CPU: not zero-copy.
                            const bool useYuv=yuvOutput.load() && !(p.width&1) && !(p.height&1) &&
                                source->color_range!=AVCOL_RANGE_JPEG &&
                                (source->colorspace==AVCOL_SPC_UNSPECIFIED || source->colorspace==AVCOL_SPC_BT709 ||
                                 source->colorspace==AVCOL_SPC_SMPTE170M || source->colorspace==AVCOL_SPC_BT470BG) &&
                                (source->format==AV_PIX_FMT_NV12 || source->format==AV_PIX_FMT_YUV420P);
                            auto pixels=std::make_shared<Bytes>(size_t(p.width)*p.height*(useYuv?3:8)/2);
                            p.pixels=pixels;
                            if(useYuv) {
                                p.pixelFormat=SDL_PIXELFORMAT_NV12;
                                p.yuvMode=(source->colorspace==AVCOL_SPC_BT709 ||
                                    (source->colorspace==AVCOL_SPC_UNSPECIFIED && p.height>576)) ?
                                    SDL_YUV_CONVERSION_BT709:SDL_YUV_CONVERSION_BT601;
                                auto *y=pixels->data();auto *uv=y+size_t(p.width)*p.height;
                                for(int row=0;row<p.height;++row)
                                    std::memcpy(y+size_t(row)*p.width,source->data[0]+ptrdiff_t(row)*source->linesize[0],p.width);
                                for(int row=0;row<p.height/2;++row) {
                                    auto *out=uv+size_t(row)*p.width;
                                    if(source->format==AV_PIX_FMT_NV12)
                                        std::memcpy(out,source->data[1]+ptrdiff_t(row)*source->linesize[1],p.width);
                                    else {
                                        const auto *u=source->data[1]+ptrdiff_t(row)*source->linesize[1];
                                        const auto *v=source->data[2]+ptrdiff_t(row)*source->linesize[2];
                                        for(int col=0;col<p.width/2;++col) {out[col*2]=u[col];out[col*2+1]=v[col];}
                                    }
                                }
                                converted=true;
                            } else {
                                sws=sws_getCachedContext(sws,p.width,p.height,AVPixelFormat(source->format),
                                    p.width,p.height,AV_PIX_FMT_RGBA,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
                                if(sws) {
                                    uint8_t *dst[]={pixels->data()};int stride[]={p.width*4};
                                    converted=sws_scale(sws,source->data,source->linesize,0,p.height,dst,stride)==p.height;
                                }
                            }
                        }
                        if(converted) {
                            std::lock_guard lock(mutex);
                            if (mediaPublishAllowed(currentGeneration,generation.load(),
                                                     currentRecovery,recovery.load())) {
                                if (diag_ && p.pixels && (!diagnosticPicture_.pixels || serial == 60))
                                    diagnosticPicture_ = p;
                                p.serial = ++serial;
                                ++publishedTotal;
                                if(p.hardwareFrame) ++nativePublished;
                                width = p.width;
                                height = p.height;
                                lastDecodedMs = now;
                                if(!pacing) lastFrameMs = now;
                                if (!damaged)
                                    ++cleanFrames;
                                ++fpsFrames;
                                if (fpsWindow == 0)
                                    fpsWindow = now;
                                if (now - fpsWindow >= 1000) {
                                    fps = double(fpsFrames) * 1000.0 / double(now - fpsWindow);
                                    fpsFrames = 0;
                                    fpsWindow = now;
                                }
                                if(pacing) {
                                    // Deadline selection is applied in pushPlayout;
                                    // source timestamps retain only ordering duties.
                                    if(p.mediaTimestamp==AV_NOPTS_VALUE) ++missingTimestamps;
                                    if(lastPresentedTimestamp!=AV_NOPTS_VALUE && p.mediaTimestamp!=AV_NOPTS_VALUE &&
                                       p.mediaTimestamp<=lastPresentedTimestamp) {
                                        tracePicture(p,now,"stale_timestamp");
                                        ++presentationSkipped;
                                    } else {
                                        pushPlayout(std::move(p));
                                    }
                                } else latest = std::move(p);
                            }
                        }
                    }
                    av_frame_unref(frame);
                }
            };
            const auto decodeStart = std::chrono::steady_clock::now();
            auto timedSend = [&](AVPacket *value) {
                const auto sendStarted=std::chrono::steady_clock::now();
                const int sent=avcodec_send_packet(ctx,value);
                sendNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()-sendStarted).count();
                return sent;
            };
            int result = timedSend(packet);
            if (result == AVERROR(EAGAIN)) {
                drain();
                result = timedSend(packet);
            }
            if (result >= 0)
                drain();
            else {
                std::lock_guard lock(mutex);
                if(mediaPublishAllowed(currentGeneration,generation.load(),
                                       currentRecovery,recovery.load())) {
                    ++decodeErrors;
                    keyframeRequested = true;
                }
            }
            const auto decodeMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - decodeStart)
                                          .count();
            const double millis = double(decodeMicros) / 1000.0;
            std::lock_guard lock(mutex);
            if(!mediaPublishAllowed(currentGeneration,generation.load(),
                                     currentRecovery,recovery.load())) continue;
            atomicMax(decodeMaxUs, uint64_t(std::max<int64_t>(0,decodeMicros)));
            if(decodeMicros>8000) ++decodeGt8;
            if(decodeMicros>16000) ++decodeGt16;
            if(decodeMicros>33000) ++decodeGt33;
            const double previous = decodeMs.load();
            decodeMs = previous <= 0 ? millis : previous + (millis - previous) * kDecodeStageEmaAlpha;
            stageSendUs=ema(stageSendUs,double(sendNs)/1000.0);
            stageRecvUs=ema(stageRecvUs,double(recvNs)/1000.0);
            stagePublishUs=ema(stagePublishUs,double(publishNs)/1000.0);
#ifdef __SWITCH__
            LiveTelemetry::setDecodeStages(stageSendUs,stageRecvUs,stagePublishUs);
#endif
        }
    done:
        return;
    }
};
Media::Media() : impl_(std::make_unique<Impl>()) {}
Media::~Media() = default;
void Media::video(const uint8_t *data, size_t size, uint64_t generation, std::optional<uint32_t> timestamp, bool hevc,
                  std::optional<bool> validatedKeyframe) {
    if (!size || !data || impl_->decoderFailed.load() || generation != impl_->generation.load())
        return;
    // Allocate the FFmpeg-owned padded packet once, before taking the shared
    // queue/UI mutex. Ownership moves to the decoder without another AU copy.
    // Match receiver's 4MB limit for 1080p high bitrate support.
    constexpr size_t maxFrameSize = 4 * 1024 * 1024;
    Impl::PacketRef owned;
    if (size <= maxFrameSize) {
        owned.reset(av_packet_alloc());
        if (!owned || av_new_packet(owned.get(),int(size))<0) return;
        std::copy(data,data+size,owned->data);
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (generation != impl_->generation.load())
            return;
        // Count only accepted-generation submissions; stale callbacks must not
        // contaminate a new epoch's AU/queue diagnostics.
        impl_->auBytesTotal.fetch_add(size, std::memory_order_relaxed);
        impl_->auCountDiag.fetch_add(1, std::memory_order_relaxed);
        atomicMax(impl_->auMaxBytes, size);
        if (size > maxFrameSize) {
            impl_->auOversize.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // A too-deep queue means decoding is behind: drop the backlog and ask
        // for a fresh IDR instead of feeding broken references to the decoder.
        constexpr size_t maxQueuedFrames = 32;
        constexpr size_t maxQueuedBytes = 8u * 1024u * 1024u;
        const bool bytesLimit = impl_->queueBytes > maxQueuedBytes ||
            size > maxQueuedBytes - impl_->queueBytes;
        if (impl_->queue.size() >= maxQueuedFrames || bytesLimit) {
            impl_->droppedUnits += impl_->queue.size();
            impl_->queue.clear();impl_->queueBytes=0;
            ++impl_->recovery;
            impl_->clear = true;
            impl_->keyframeRequested = true;
            impl_->presentationSkipped+=impl_->ready.size();impl_->resetPlayout();
        }
        int64_t extended=AV_NOPTS_VALUE;
        if(timestamp) {
            extended=impl_->haveInputTimestamp ? impl_->highestInputTimestamp+
                int32_t(*timestamp-uint32_t(impl_->highestInputTimestamp)) : int64_t(*timestamp);
            if(!impl_->haveInputTimestamp || extended>impl_->highestInputTimestamp) impl_->highestInputTimestamp=extended;
            impl_->haveInputTimestamp=true;
        }
        impl_->queue.push_back(Impl::EncodedUnit{std::move(owned),size,extended,hevc,mediaClockMs(),validatedKeyframe});
        impl_->queueBytes += size;
        impl_->queueHighFrames = std::max(impl_->queueHighFrames, impl_->queue.size());
        impl_->queueHighBytes = std::max(impl_->queueHighBytes, impl_->queueBytes);
    }
    impl_->cv.notify_one();
}
namespace {
int opusFrameSamples(int samples) {
    switch (samples) {
    case 120: case 240: case 480: case 960: case 1920: case 2880: return samples;
    default: return 960;
    }
}
}
void Media::Impl::resetAudioSchedule() {
    audioHold.clear();
    audioAnchored=false;audioHaveClock=false;audioHavePlay=false;
    audioAnchorRtp=audioAnchorDueMs=audioExtendedRtp=audioPlayRtp=0;
    audioLastRtp32=0;audioFrameSamples=960;
}
void Media::Impl::queueAudioPcm(int samples) {
    if(samples<=0 || !audioDevice) return;
    const int volume=volumePercent.load();
    if(volume!=100) for(int i=0;i<samples*2;++i) audioPcm[size_t(i)]=int16_t(int(audioPcm[size_t(i)])*volume/100);
    // A device queue past 200 ms is already late. Drop this frame and leave
    // the samples that are still playing.
    if(SDL_GetQueuedAudioSize(audioDevice) > 48000u * 4u / 5u) return;
    audioStatus=SDL_QueueAudio(audioDevice,audioPcm.data(),Uint32(samples*4))==0 ?
        AudioStatus::Ready : AudioStatus::QueueFailed;
}
void Media::Impl::concealAudioUntil(int64_t rtp) {
    const int frame=opusFrameSamples(audioFrameSamples);
    if(!audioHavePlay) {
        audioPlayRtp=rtp;audioHavePlay=true;return;
    }
    if(rtp<=audioPlayRtp+frame/2) return;
    int concealed=0;
    while(audioPlayRtp+frame/2<rtp && concealed<frame*3) {
        const int samples=opus_decode(opus,nullptr,0,audioPcm.data(),frame,0);
        if(samples<=0) break;
        queueAudioPcm(samples);
        audioPlayRtp+=samples;concealed+=samples;audioFrameSamples=samples;
    }
    if(rtp-audioPlayRtp>opusFrameSamples(audioFrameSamples)) {
        opus_decoder_ctl(opus,OPUS_RESET_STATE);
        audioPlayRtp=rtp;
    }
}
void Media::Impl::releaseAudio() {
    const auto now=mediaClockMs();
    while(!audioHold.empty()) {
        const auto it=audioHold.begin();
        const int64_t due=audioAnchorDueMs+(it->first-audioAnchorRtp)*1000/48000;
        if(due>now) break;
        const int frame=opusFrameSamples(audioFrameSamples);
        if(audioHavePlay && it->first+frame/2<audioPlayRtp) {audioHold.erase(it);continue;}
        concealAudioUntil(it->first);
        const int samples=opus_decode(opus,it->second.data(),int(it->second.size()),audioPcm.data(),5760,0);
        if(samples>0) {
            audioFrameSamples=samples;
            queueAudioPcm(samples);
            audioPlayRtp=it->first+samples;audioHavePlay=true;
        }
        audioHold.erase(it);
    }
    while(audioHold.size()>16) audioHold.erase(audioHold.begin());
}
void Media::Impl::ingestAudio(const uint8_t *data, size_t size, std::optional<uint32_t> timestamp) {
    if(!timestamp) {
        const int samples=opus_decode(opus,data,int(size),audioPcm.data(),5760,0);
        if(samples>0) {audioFrameSamples=samples;queueAudioPcm(samples);}
        return;
    }
    int64_t extended;
    if(!audioHaveClock) {
        audioHaveClock=true;audioLastRtp32=*timestamp;audioExtendedRtp=int64_t(*timestamp);extended=audioExtendedRtp;
    } else {
        const int32_t delta=int32_t(*timestamp-audioLastRtp32);
        audioLastRtp32=*timestamp;audioExtendedRtp+=delta;extended=audioExtendedRtp;
    }
    const int frame=opusFrameSamples(audioFrameSamples);
    if(audioHavePlay && extended+frame/2<audioPlayRtp) return;
    if(!audioAnchored) {
        audioAnchored=true;audioAnchorRtp=extended;audioAnchorDueMs=mediaClockMs()+audioDelayMs;
    } else if(!audioHavePlay && extended<audioAnchorRtp) {
        audioAnchorDueMs-=(audioAnchorRtp-extended)*1000/48000;audioAnchorRtp=extended;
        const auto now=mediaClockMs();
        if(audioAnchorDueMs<now) audioAnchorDueMs=now;
    }
    if(!audioHold.count(extended)) audioHold.emplace(extended,std::vector<uint8_t>(data,data+size));
    releaseAudio();
}
void Media::audio(const uint8_t *data, size_t size, uint64_t generation, std::optional<uint32_t> timestamp) {
    if (!size || size > 4096)
        return;
    std::lock_guard lock(impl_->audioMutex);
    if (generation != impl_->generation.load())
        return;
    impl_->ingestAudio(data, size, timestamp);
}
Picture Media::picture(uint64_t after, int presentationLeadMs) {
    std::lock_guard lock(impl_->mutex);
    (void)presentationLeadMs;
    if(impl_->pacing)
        return impl_->nextPlayout(after,mediaClockMs());
    return impl_->latest.serial > after ? impl_->latest : Picture{};
}
void Media::setYuvOutput(bool enabled) { impl_->yuvOutput.store(enabled); }
void Media::setNativeOutput(bool enabled) { impl_->nativeOutput.store(enabled); }
void Media::setPlayoutDelay(int milliseconds) {
    std::lock_guard lock(impl_->mutex);
    milliseconds=std::clamp(milliseconds,0,100);
    if(impl_->playoutDelayMs==milliseconds) return;
    const int previous=impl_->playoutDelayMs;
    const auto now=mediaClockMs();
    for(const auto &p:impl_->ready) impl_->tracePicture(p,now,"settings_changed");
    impl_->presentationSkipped+=impl_->ready.size();
    impl_->resetPlayout();
    // Delay is a presentation setting; pacing remains independently controlled
    // by frame_pacing (Impl::pacing). Do not turn pacing on from this setter.
    impl_->playoutDelayMs=milliseconds;
    std::lock_guard audioLock(impl_->audioMutex);
    impl_->audioDelayMs=milliseconds;
    if(impl_->audioAnchored) impl_->audioAnchorDueMs+=milliseconds-previous;
    impl_->releaseAudio();
}
void Media::reset() {
    const Uint32 resetStarted=SDL_GetTicks();
    Impl::DiagnosticDump diagnosticDump;
    {
        std::lock_guard lock(impl_->mutex);
        // Move immutable diagnostic ownership before clearing the session.
        // No codec wait, file I/O or image conversion under the shared lock.
        diagnosticDump=impl_->takeDiagnostics();
        impl_->generation.fetch_add(1);
        ++impl_->recovery;
        impl_->auDump_.clear();
        impl_->auCount_ = 0;
        impl_->diagnosticPicture_ = {};
        impl_->keyframeCount = 0;
        impl_->decodedTotal=0;impl_->publishedTotal=0;impl_->hardwareFrame=false;
        impl_->cpuTransfers=0;impl_->nativePublished=0;
        impl_->queue.clear();
        impl_->latest = {};
        impl_->resetPlayout();
        impl_->haveInputTimestamp=false;
        impl_->lastPresentedTimestamp=AV_NOPTS_VALUE;
        impl_->presentationSkipped=0;impl_->clockResets=0;impl_->missingTimestamps=0;
        impl_->pictureEmpty=0;impl_->pictureEmptyNoReady=0;impl_->pictureEmptyFuture=0;
        impl_->pictureSelected=0;impl_->pictureFutureGapMaxMs=0;
        impl_->selectedAgeTotalMs=0;impl_->selectedAgeSamples=0;impl_->selectedAgeMaxMs=0;
        impl_->decodeToQueueTotalMs=0;impl_->queueWaitTotalMs=0;impl_->ageBreakdownSamples=0;
        impl_->skipDue=0;impl_->skipBound=0;impl_->selectedLateMs=0;
        impl_->playoutTrace.clear();
        impl_->clear = true;
        impl_->width = 0;
        impl_->height = 0;
        impl_->lastFrameMs = 0;
        impl_->lastDecodedMs = 0;
        impl_->auBytesTotal=0;impl_->auMaxBytes=0;impl_->auOversize=0;impl_->auCountDiag=0;impl_->auCount_ = 0;
        impl_->queueBytes=0;impl_->queueHighFrames=0;impl_->queueHighBytes=0;
        impl_->queueWaitLastMs=0;impl_->queueWaitMaxMs=0;
        impl_->decodeMaxUs=0;impl_->decodeGt8=0;impl_->decodeGt16=0;impl_->decodeGt33=0;
        impl_->cleanFrames = 0;
        impl_->decodeErrors = 0;
        impl_->droppedUnits = 0;
        impl_->keyframeRequested = true;
        impl_->fps = 0;
        impl_->decodeMs = 0;
#ifdef __SWITCH__
        LiveTelemetry::clearDecodeStages();
#endif
        impl_->fpsFrames = 0;
        impl_->fpsWindow = 0;
        impl_->codecText = "—";
        impl_->hintedCodecPublished_=false;
        impl_->encoderText = "—";
    }
    impl_->queueDiagnostics(std::move(diagnosticDump));
    diagnostic(("media-reset-flush-ms-"+std::to_string(SDL_GetTicks()-resetStarted)).c_str());
    diagnostic(("media-reset-trace-queued-ms-"+std::to_string(SDL_GetTicks()-resetStarted)).c_str());
    impl_->cv.notify_one();

    // Wait for decoder thread to observe the generation change with timeout.
    // Prevents indefinite blocking if GPU hangs or decoder is stuck in long operation.
    // The decoder thread checks generation in its loop; once it observes clear=true
    // and new generation, it will drain the queue and wait on cv.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    unsigned spinCount = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        // Check if decoder has acknowledged the reset by clearing the clear flag
        // or if the queue is empty (decoder consumed all old work)
        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->clear) {
                // Decoder thread has acknowledged reset by setting clear=false
                break;
            }
        }
        if (++spinCount >= 50) {
            // After 500ms, log timeout but continue. Generation check in decoder
            // will eventually discard old work. This prevents user-visible hang.
            diagnostic("media-reset-timeout-decoder-busy");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const Uint32 audioLockStarted=SDL_GetTicks();
    std::unique_lock audioLock(impl_->audioMutex);
    diagnostic(("media-reset-audio-lock-ms-"+std::to_string(SDL_GetTicks()-audioLockStarted)).c_str());
    const Uint32 audioClearStarted=SDL_GetTicks();
    impl_->resetAudioSchedule();
    if (impl_->audioDevice)
        SDL_ClearQueuedAudio(impl_->audioDevice);
    diagnostic(("media-reset-audio-clear-ms-"+std::to_string(SDL_GetTicks()-audioClearStarted)).c_str());
    const Uint32 opusStarted=SDL_GetTicks();
    opus_decoder_ctl(impl_->opus, OPUS_RESET_STATE);
    diagnostic(("media-reset-opus-ms-"+std::to_string(SDL_GetTicks()-opusStarted)).c_str());
    diagnostic(("media-reset-total-ms-"+std::to_string(SDL_GetTicks()-resetStarted)).c_str());
}
uint64_t Media::generation() const {
    return impl_->generation.load();
}
std::string Media::status() {
    std::lock_guard lock(impl_->mutex);
    std::string text = impl_->error.empty() ? (impl_->hardware ? "NVDEC 硬件解码" : "软件解码") : impl_->error;
    const int width = impl_->width.load(), height = impl_->height.load();
    if (width > 0)
        text += " · " + std::to_string(width) + "x" + std::to_string(height);
    const auto errors = impl_->decodeErrors.load(), dropped = impl_->droppedUnits.load();
    if (errors)
        text += " · 解码错误 " + std::to_string(errors);
    if (dropped)
        text += " · 丢帧 " + std::to_string(dropped);
    if(impl_->decoderFailed.load()) text+=" · 视频解码已停止，请退出程序后重试";
    const auto audio=impl_->audioStatus.load();
    if(audio!=Impl::AudioStatus::Ready)
        text+=audio==Impl::AudioStatus::Unavailable ? " · 音频设备不可用，请退出程序后重试":" · 音频播放失败，正在重试";
    return text;
}
bool Media::takeKeyframeRequest() {
    return impl_->keyframeRequested.exchange(false);
}
int64_t Media::frameAgeMs() {
    const auto last = impl_->lastDecodedMs.load();
    return last == 0 ? int64_t{1} << 40 : mediaClockMs() - last;
}
MediaStats Media::stats() {
    MediaStats out;
    std::lock_guard lock(impl_->mutex);
    out.decodedTotal=impl_->decodedTotal.load();out.publishedTotal=impl_->publishedTotal.load();
    out.epoch=impl_->generation.load();out.inputQueued=int(impl_->queue.size());
    out.hardwareFrame=impl_->hardwareFrame.load();
    out.cpuTransfers=impl_->cpuTransfers.load();out.nativePublished=impl_->nativePublished.load();
    out.skipDue=impl_->skipDue;out.skipBound=impl_->skipBound;out.selectedLateMs=impl_->selectedLateMs;
    out.codec = impl_->codecText;
    out.encoder = impl_->encoderText;
    out.decoder = impl_->hardware ? "NVDEC 硬解" : "软件解码（FFmpeg）";
    out.width = impl_->width.load();
    out.height = impl_->height.load();
    out.fps = impl_->fps.load();
    out.decodeMs = impl_->decodeMs.load();
    out.decodeErrors = impl_->decodeErrors.load();
    out.droppedFrames = impl_->droppedUnits.load();
    out.keyframeCount = impl_->keyframeCount.load();
    out.pacing=impl_->pacing;out.playoutDelayMs=impl_->playoutDelayMs;
    out.readyFrames=int(impl_->ready.size());out.presentationSkipped=impl_->presentationSkipped;
    out.pictureEmpty=impl_->pictureEmpty;out.pictureEmptyNoReady=impl_->pictureEmptyNoReady;
    out.pictureEmptyFuture=impl_->pictureEmptyFuture;out.pictureSelected=impl_->pictureSelected;
    out.pictureFutureGapMaxMs=impl_->pictureFutureGapMaxMs;
    out.selectedAgeTotalMs=impl_->selectedAgeTotalMs;out.selectedAgeSamples=impl_->selectedAgeSamples;
    out.selectedAgeMaxMs=impl_->selectedAgeMaxMs;
    out.decodeToQueueTotalMs=impl_->decodeToQueueTotalMs;
    out.queueWaitTotalMs=impl_->queueWaitTotalMs;
    out.ageBreakdownSamples=impl_->ageBreakdownSamples;
    out.clockResets=impl_->clockResets;out.missingTimestamps=impl_->missingTimestamps;
    out.auMaxBytes=impl_->auMaxBytes.load();out.auBytesTotal=impl_->auBytesTotal.load();
    out.auCount=impl_->auCountDiag.load();out.auOversize=impl_->auOversize.load();
    out.queueHighFrames=impl_->queueHighFrames;out.queueHighBytes=impl_->queueHighBytes;
    out.queueWaitLastMs=int(impl_->queueWaitLastMs);out.queueWaitMaxMs=int(impl_->queueWaitMaxMs);
    out.oldestQueueWaitMs=impl_->queue.empty()?0:int(std::max<int64_t>(0,mediaClockMs()-impl_->queue.front().enqueuedMs));
    out.decodeMaxMs=double(impl_->decodeMaxUs.load())/1000.0;
    out.decodeGt8=impl_->decodeGt8.load();out.decodeGt16=impl_->decodeGt16.load();out.decodeGt33=impl_->decodeGt33.load();
    const auto last=impl_->lastDecodedMs.load();
    out.lastDecodedAgeMs=last?mediaClockMs()-last:-1;
    return out;
}
uint64_t Media::keyframeCount() {
    return impl_->keyframeCount.load();
}
} // namespace uu

void uu::Media::setVolume(int percent) {impl_->volumePercent.store(std::clamp(percent,0,100));}
