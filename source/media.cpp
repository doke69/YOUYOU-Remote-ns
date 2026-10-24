#include "cpu_diagnostics.hpp"
#include "media.hpp"
#include "config_store.hpp"
#include "diagnostics.hpp"
#ifdef __SWITCH__
#include "live_telemetry.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
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
        sws_freeContext(scaler);av_packet_free(&packet);
        av_frame_free(&cpu);av_frame_free(&frame);avcodec_free_context(&context);
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
    std::mutex mutex, audioMutex, decodeStateMutex;
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
    bool pacing = mediaConfigInt("frame_pacing",1,0,1)!=0;
    int playoutDelayMs = mediaConfigInt("playout_delay_ms",35,0,100);
    bool haveInputTimestamp = false, havePlayoutClock = false;
    int64_t highestInputTimestamp = 0, clockTimestamp = 0, clockWallMs = 0;
    int64_t lastPresentedTimestamp = AV_NOPTS_VALUE;
    uint64_t presentationSkipped = 0, clockResets = 0, missingTimestamps = 0;
    uint64_t pictureEmpty = 0, pictureEmptyNoReady = 0, pictureEmptyFuture = 0;
    uint64_t pictureSelected = 0;
    int pictureFutureGapMaxMs = 0;
    uint64_t skipDue = 0, skipBound = 0;
    int selectedLateMs = 0;
    double framePeriodMs = 16.667;
    int64_t cadenceTimestamp = AV_NOPTS_VALUE;
    // Experimental presentation-only correction; RTP and decoded pixels stay intact.
    int cadenceSmoothingMs=mediaConfigInt("cadence_smoothing_ms",0,0,4);
    int64_t smoothedTimestamp=AV_NOPTS_VALUE;
    double smoothedDueMs=0;
    uint64_t smoothedFrames=0;
    int maxDueShiftMs=0;
    std::deque<std::string> playoutTrace;
    void tracePicture(const Picture &picture, int64_t now, const char *reason, int64_t targetMs = 0) {
        // Bounded numeric-only trace, independent of the AU/video dump switch.
        // 6000 rows (~60-100 s at 60 fps) is still a few hundred KiB and makes
        // the presentation-drop trigger analyzable after a reset.
        if(playoutTrace.size()>=6000) playoutTrace.pop_front();
        playoutTrace.push_back(std::to_string(picture.mediaTimestamp)+","+std::to_string(picture.decodedMs)+","+
            std::to_string(picture.dueMs)+","+std::to_string(now)+","+reason+","+std::to_string(targetMs?targetMs:now)+","+std::to_string(picture.originalDueMs));
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
    Impl() {
      try {
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
    void flushDiagnostics() {
        const bool haveTiming = !playoutTrace.empty();
        const bool haveMediaDump = diag_ && !auDump_.empty();
        if (!haveTiming && !haveMediaDump)
            return;
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
            for(const auto &row:playoutTrace) timing << row << '\n';
        }
        if (!haveMediaDump)
            return;
        std::ofstream file(std::string(diagDirectory()) + (diagnosticHevc_?"/video.h265":"/video.h264"), std::ios::binary);
        if (file)
            file.write(reinterpret_cast<const char *>(auDump_.data()), std::streamsize(auDump_.size()));
        std::ofstream info(std::string(diagDirectory()) + "/media_info.txt");
        // Snapshot after conversion and before SDL upload: distinguishes the
        // decoded image from a renderer/texture stride or synchronisation issue.
        if (diagnosticPicture_.pixels) {
            const auto &p = diagnosticPicture_;
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
            info << "aus=" << auCount_ << "\n";
            info << "dump_bytes=" << auDump_.size() << "\n";
            info << "decoder=" << (hardware ? "nvdec" : "software") << "\n";
            info << "codec=" << codecText << "\n";
            info << "encoder=" << encoderText << "\n";
            info << "width=" << width.load() << "\n";
            info << "height=" << height.load() << "\n";
            info << "decode_errors=" << decodeErrors.load() << "\n";
            info << "dropped=" << droppedUnits.load() << "\n";
            info << "keyframes=" << keyframeCount.load() << "\n";
            info << "fps=" << fps.load() << "\n";
            info << "decode_ms=" << decodeMs.load() << "\n";
            info << "frame_pacing=" << pacing << "\nplayout_delay_ms=" << playoutDelayMs
                 << "\nready_frames=" << ready.size() << "\npresentation_skipped=" << presentationSkipped
                 << "\nplayout_clock_resets=" << clockResets << "\nmissing_frame_timestamps=" << missingTimestamps << '\n';
            info << "cadence_smoothing_ms=" << cadenceSmoothingMs << "\nsmoothed_frames=" << smoothedFrames
                 << "\nmax_due_shift_ms=" << maxDueShiftMs << '\n';
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
    void inspect(const uint8_t *data, size_t length, bool hevc, std::optional<bool> validatedKeyframe) {
        diagnosticHevc_=hevc;
        if (length < 5) return;
        if (hevc && validatedKeyframe.has_value()) {
            // The strict assembler already parsed every NAL and proved this AU.
            // This only avoids a second scan for UI diagnostics; FFmpeg still
            // parses/validates the encoded packet normally.
            if (!hintedCodecPublished_) {
                std::lock_guard lock(mutex);
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
        if (!hevc) hintedCodecPublished_=false;
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
        if (diag_) {
            ++auCount_;
            if (auDump_.size() + length <= auLimit_)
                auDump_.insert(auDump_.end(), data, data + length);
        }
        if (idr)
            ++keyframeCount;
        if (!codec.empty() || !encoder.empty()) {
            std::lock_guard lock(mutex);
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
        auto ema=[](double value,double sample){return value<=0?sample:value+(sample-value)*0.15;};
        // Session epoch for the local stage EMAs. Media::reset() increments
        // generation() while holding decodeStateMutex; this loop re-reads it
        // under the same lock, so a reconnect cannot blend old-session EMAs.
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
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop || clear || !queue.empty(); });
                if (stop)
                    break;
                if (clear) {
                    avcodec_flush_buffers(ctx);
                    clear = false;
                }
                if (queue.empty())
                    continue;
                unit = std::move(queue.front());
                queue.pop_front();
                if(unit.byteSize<=queueBytes) queueBytes-=unit.byteSize; else queueBytes=0;
                const auto dequeuedMs=mediaClockMs();
                queueWaitLastMs=unit.enqueuedMs?std::max<int64_t>(0,dequeuedMs-unit.enqueuedMs):0;
                queueWaitMaxMs=std::max(queueWaitMaxMs,queueWaitLastMs);
                currentGeneration = generation.load();
            }
            // Serialize reset/diagnostic snapshots with the decode unit. Do
            // not hold the queue mutex across codec calls.
            std::lock_guard decodeStateLock(decodeStateMutex);
            const uint64_t activeGeneration=generation.load();
            if(activeGeneration!=stageEpoch) {
                // New session: clear local EMA state. reset() already cleared
                // the published atomics under this same lock, so the next
                // publish() must not republish blended values.
                stageSendUs=stageRecvUs=stagePublishUs=0;
                stageEpoch=activeGeneration;
            }
            if (currentGeneration != activeGeneration)
                continue;
            const auto wanted=unit.hevc?AV_CODEC_ID_HEVC:AV_CODEC_ID_H264;
            if(wanted!=activeCodec) {
                av_frame_unref(frame);av_frame_unref(cpu);av_packet_unref(packet);
                sws_freeContext(sws);sws=nullptr;
                if(!openDecoder(ctx,wanted)) {decoderFailed=true;goto done;}
                activeCodec=wanted;
                std::lock_guard lock(mutex);
                presentationSkipped+=ready.size();ready.clear();readyBytes=0;
                havePlayoutClock=false;codecText=unit.hevc?"H.265 Main":"H.264";
            }
            uu_cpu_mark(1);
            av_packet_unref(packet);
            av_packet_move_ref(packet,unit.packet.get());
            inspect(packet->data,size_t(packet->size),unit.hevc,unit.validatedKeyframe);
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
                    if(receiveResult!=0) break;
                    StageTimer publishTimer(publishNs,frameReceived);
                    ++decodedTotal;
#ifdef __SWITCH__
                    hardwareFrame=frame->format==AV_PIX_FMT_NVTEGRA;
#else
                    hardwareFrame=false;
#endif
                    const bool damaged = frame->decode_error_flags != 0;
                    if (damaged) {
                        ++decodeErrors;
                        keyframeRequested = true;
                    }
                    AVFrame *source = frame;
#ifdef __SWITCH__
                    if (frame->format == AV_PIX_FMT_NVTEGRA && !nativeOutput.load()) {
                        av_frame_unref(cpu);
                        ++cpuTransfers;
                        if (av_hwframe_transfer_data(cpu, frame, 0) < 0) {
                            ++decodeErrors;
                            keyframeRequested = true;
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
                    const bool show = !damaged || (!strictReceiver_ && (cleanFrames == 0 || now - lastFrameMs.load() > 2000));
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
                            if (diag_ && (!diagnosticPicture_.pixels || serial == 60))
                                diagnosticPicture_ = p;
                            std::lock_guard lock(mutex);
                            if (currentGeneration == generation.load()) {
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
                                    if(p.mediaTimestamp!=AV_NOPTS_VALUE) {
                                        if(cadenceTimestamp!=AV_NOPTS_VALUE) {
                                            const auto period=(p.mediaTimestamp-cadenceTimestamp)/90.0;
                                            if(period>=8 && period<=50) framePeriodMs=framePeriodMs*.9+period*.1;
                                        }
                                        cadenceTimestamp=p.mediaTimestamp;
                                    }
                                    if(p.mediaTimestamp==AV_NOPTS_VALUE || playoutDelayMs==0) {
                                        if(p.mediaTimestamp==AV_NOPTS_VALUE) ++missingTimestamps;
                                        // Interactive mode: complete decoded frames become
                                        // eligible immediately; VSync is a separate setting.
                                        p.dueMs=now;
                                    } else {
                                        if(!havePlayoutClock) {
                                            clockTimestamp=p.mediaTimestamp;clockWallMs=now+playoutDelayMs;
                                            smoothedTimestamp=AV_NOPTS_VALUE;
                                            havePlayoutClock=true;
                                        }
                                        p.dueMs=clockWallMs+(p.mediaTimestamp-clockTimestamp)/90;
                                        if(p.dueMs < now-120) {
                                            // A long pause/recovery starts a fresh presentation epoch.
                                            presentationSkipped+=ready.size();ready.clear();readyBytes=0;
                                            clockTimestamp=p.mediaTimestamp;clockWallMs=now+playoutDelayMs;
                                            p.dueMs=clockWallMs;++clockResets;smoothedTimestamp=AV_NOPTS_VALUE;
                                        } else if(p.dueMs > now+playoutDelayMs+30) {
                                            // Don't retain the slow first IDR's receive delay forever.
                                            const auto advance=p.dueMs-(now+playoutDelayMs);
                                            clockWallMs-=advance;p.dueMs-=advance;
                                            for(auto &queued:ready) {queued.dueMs-=advance;queued.originalDueMs-=advance;}
                                            smoothedDueMs-=advance;
                                        }
                                    }
                                    p.originalDueMs=p.dueMs;
                                    if(cadenceSmoothingMs>0 && playoutDelayMs>0 && p.mediaTimestamp!=AV_NOPTS_VALUE &&
                                       (lastPresentedTimestamp==AV_NOPTS_VALUE || p.mediaTimestamp>lastPresentedTimestamp)) {
                                        const auto step=smoothedTimestamp==AV_NOPTS_VALUE?0.0:
                                            (p.mediaTimestamp-smoothedTimestamp)/90.0;
                                        if(step>=8 && step<=50) {
                                            const auto predicted=smoothedDueMs+framePeriodMs;
                                            const auto error=double(p.originalDueMs)-predicted;
                                            const auto phase=predicted+std::clamp(error,-8.0,8.0)*.125;
                                            // Never shift more than the configured4ms, add queue
                                            // capacity, or keep stale frames to force a frame rate.
                                            const auto limit=double(cadenceSmoothingMs);
                                            p.dueMs=std::llround(std::clamp(phase,
                                                double(p.originalDueMs)-limit,double(p.originalDueMs)+limit));
                                            const auto shift=int(std::abs(p.dueMs-p.originalDueMs));
                                            if(shift) ++smoothedFrames;
                                            maxDueShiftMs=std::max(maxDueShiftMs,shift);
                                        }
                                        smoothedDueMs=double(p.dueMs);smoothedTimestamp=p.mediaTimestamp;
                                    } else smoothedTimestamp=AV_NOPTS_VALUE;
                                    if(lastPresentedTimestamp!=AV_NOPTS_VALUE && p.mediaTimestamp!=AV_NOPTS_VALUE &&
                                       p.mediaTimestamp<=lastPresentedTimestamp) {
                                        tracePicture(p,now,"stale_timestamp");
                                        ++presentationSkipped;
                                    } else {
                                        readyBytes+=p.pixelBytes();
                                        auto pos=std::upper_bound(ready.begin(),ready.end(),p.dueMs,
                                            [](int64_t due,const Picture &candidate){return due<candidate.dueMs;});
                                        ready.insert(pos,std::move(p));
                                        while(ready.size()>4 || readyBytes>32u*1024u*1024u) {
                                            tracePicture(ready.front(),now,"queue_bound");++skipBound;
                                            readyBytes-=ready.front().pixelBytes();ready.pop_front();++presentationSkipped;
                                        }
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
                ++decodeErrors;
                keyframeRequested = true;
            }
            const auto decodeMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - decodeStart)
                                          .count();
            const double millis = double(decodeMicros) / 1000.0;
            atomicMax(decodeMaxUs, uint64_t(std::max<int64_t>(0,decodeMicros)));
            if(decodeMicros>8000) ++decodeGt8;
            if(decodeMicros>16000) ++decodeGt16;
            if(decodeMicros>33000) ++decodeGt33;
            const double previous = decodeMs.load();
            decodeMs = previous <= 0 ? millis : previous + (millis - previous) * 0.15;
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
    Impl::PacketRef owned;
    if (size <= 2 * 1024 * 1024) {
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
        if (size > 2 * 1024 * 1024) {
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
            impl_->clear = true;
            impl_->keyframeRequested = true;
            impl_->presentationSkipped+=impl_->ready.size();impl_->ready.clear();impl_->readyBytes=0;
            impl_->havePlayoutClock=false;
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
void Media::audio(const uint8_t *data, size_t size, uint64_t generation) {
    if (!size || size > 4096)
        return;
    std::lock_guard lock(impl_->audioMutex);
    if (generation != impl_->generation.load())
        return;
    std::array<int16_t, 5760 * 2> pcm{};
    int samples = opus_decode(impl_->opus, data, int(size), pcm.data(), 5760, 0);
    if (samples > 0 && impl_->audioDevice) {
        const int volume=impl_->volumePercent.load();
        if(volume!=100)for(int i=0;i<samples*2;++i)pcm[size_t(i)]=int16_t(int(pcm[size_t(i)])*volume/100);
        if (SDL_GetQueuedAudioSize(impl_->audioDevice) > 48000 * 4 / 5)
            SDL_ClearQueuedAudio(impl_->audioDevice);
        impl_->audioStatus=SDL_QueueAudio(impl_->audioDevice,pcm.data(),samples*4)==0 ?
            Impl::AudioStatus::Ready:Impl::AudioStatus::QueueFailed;
    }
}
Picture Media::picture(uint64_t after, int presentationLeadMs) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->pacing) {
        const auto now=mediaClockMs();
        while(!impl_->ready.empty() && impl_->ready.front().serial<=after) {
            impl_->tracePicture(impl_->ready.front(),now,"stale_serial");
            impl_->readyBytes-=impl_->ready.front().pixelBytes();impl_->ready.pop_front();++impl_->presentationSkipped;
        }
        // Select for the upcoming presentation, not the instant before
        // SDL uploads/draws and waits for VSync. Never wait here or extrapolate
        // an undecoded frame. One refresh interval is the maximum lookahead.
        const auto target=now+std::clamp(presentationLeadMs,0,20);
        if(impl_->ready.empty()) {
            ++impl_->pictureEmpty;++impl_->pictureEmptyNoReady;
            return {};
        }
        const auto gapMs=impl_->ready.front().dueMs-target;
        // A frame due a few milliseconds after the nominal presentation target
        // is still preferable to dropping this vsync completely. Keep the
        // allowance below half a 60 Hz frame to avoid visibly early motion.
        if(gapMs>4) {
            ++impl_->pictureEmpty;++impl_->pictureEmptyFuture;
            impl_->pictureFutureGapMaxMs=int(std::max<int64_t>(impl_->pictureFutureGapMaxMs,gapMs));
            return {};
        }
        // Preserve frame cadence when only one frame behind. Coalescing every
        // pair of eligible frames turns small scheduling jitter into sustained
        // frame loss. Catch up only beyond one source-frame period (+2ms),
        // capped at50ms; keep existing memory bounds and never block here.
        const auto lateBudget=std::clamp(impl_->framePeriodMs+2.0,12.0,50.0);
        while(impl_->ready.size()>1 && impl_->ready[1].dueMs<=target+2 &&
              target-impl_->ready.front().dueMs>lateBudget) {
            impl_->tracePicture(impl_->ready.front(),now,"superseded_due",target);++impl_->skipDue;
            impl_->readyBytes-=impl_->ready.front().pixelBytes();impl_->ready.pop_front();++impl_->presentationSkipped;
        }
        impl_->readyBytes-=impl_->ready.front().pixelBytes();
        impl_->latest=std::move(impl_->ready.front());impl_->ready.pop_front();
        ++impl_->pictureSelected;
        impl_->selectedLateMs=int(std::max<int64_t>(0,target-impl_->latest.dueMs));
        impl_->tracePicture(impl_->latest,now,"selected",target);
        impl_->lastPresentedTimestamp=impl_->latest.mediaTimestamp;
        impl_->lastFrameMs=now;
    }
    return impl_->latest.serial > after ? impl_->latest : Picture{};
}
void Media::setYuvOutput(bool enabled) { impl_->yuvOutput.store(enabled); }
void Media::setNativeOutput(bool enabled) { impl_->nativeOutput.store(enabled); }
void Media::setPlayoutDelay(int milliseconds) {
    std::lock_guard lock(impl_->mutex);
    milliseconds=std::clamp(milliseconds,0,100);
    if(impl_->playoutDelayMs==milliseconds && impl_->pacing) return;
    const auto now=mediaClockMs();
    for(const auto &p:impl_->ready) impl_->tracePicture(p,now,"settings_changed");
    impl_->presentationSkipped+=impl_->ready.size();
    impl_->ready.clear();impl_->readyBytes=0;
    impl_->pacing=true;impl_->playoutDelayMs=milliseconds;
    impl_->havePlayoutClock=false;
}
void Media::reset() {
    std::lock_guard decodeStateLock(impl_->decodeStateMutex);
    {
        std::lock_guard lock(impl_->mutex);
        // Save the completed session before clearing its live counters. Dumps
        // and receiver counters now refer to one connection, not mixed runs.
        impl_->flushDiagnostics();
        impl_->generation.fetch_add(1);
        impl_->auDump_.clear();
        impl_->auCount_ = 0;
        impl_->diagnosticPicture_ = {};
        impl_->keyframeCount = 0;
        impl_->decodedTotal=0;impl_->publishedTotal=0;impl_->hardwareFrame=false;
        impl_->cpuTransfers=0;impl_->nativePublished=0;
        impl_->queue.clear();
        impl_->latest = {};
        impl_->ready.clear();impl_->readyBytes=0;
        impl_->haveInputTimestamp=false;impl_->havePlayoutClock=false;
        impl_->lastPresentedTimestamp=AV_NOPTS_VALUE;
        impl_->presentationSkipped=0;impl_->clockResets=0;impl_->missingTimestamps=0;
        impl_->pictureEmpty=0;impl_->pictureEmptyNoReady=0;impl_->pictureEmptyFuture=0;
        impl_->pictureSelected=0;impl_->pictureFutureGapMaxMs=0;
        impl_->skipDue=0;impl_->skipBound=0;impl_->selectedLateMs=0;
        impl_->framePeriodMs=16.667;impl_->cadenceTimestamp=AV_NOPTS_VALUE;
        impl_->smoothedTimestamp=AV_NOPTS_VALUE;impl_->smoothedDueMs=0;
        impl_->smoothedFrames=0;impl_->maxDueShiftMs=0;
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
    impl_->cv.notify_one();
    std::lock_guard lock(impl_->audioMutex);
    if (impl_->audioDevice)
        SDL_ClearQueuedAudio(impl_->audioDevice);
    opus_decoder_ctl(impl_->opus, OPUS_RESET_STATE);
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
