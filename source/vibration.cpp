#include "vibration.hpp"
#include "diagnostics.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace uu::vibration {
namespace {
constexpr float kMaxAmplitude = .8f;

std::array<std::atomic<uint32_t>,2> latestPacked{};
std::array<std::atomic<int64_t>,2> latestAt{};
std::atomic<uint32_t> sessionMessages{0};
std::atomic<int64_t> sessionMaxIntervalMs{0};
std::atomic<int> strengthPercent{kDefaultStrength};
std::atomic<int> holdMs{kDefaultHoldMs};
std::atomic<bool> stopped{false};

int clampLevel(double value) {
    if (!std::isfinite(value)) return 0;
    return int(std::clamp(value, 0., 255.));
}

std::optional<Rumble> parseObject(const Json &json) {
    if (!json.is_object()) return std::nullopt;
    const auto action = json.find("action");
    if (action == json.end() || !action->is_string() || action->get_ref<const std::string &>() != "gpd_vibration")
        return std::nullopt;
    const auto index = json.find("index");
    if (index != json.end() && (!index->is_number_integer() || *index < 0 || *index > 1)) return std::nullopt;
    const auto left = json.find("left_motor"), right = json.find("right_motor");
    const bool hasLeft = left != json.end() && left->is_number();
    const bool hasRight = right != json.end() && right->is_number();
    if (!hasLeft && !hasRight) return std::nullopt;
    Rumble rumble;
    if(index!=json.end())rumble.index=index->get<int>();
    if (hasLeft) rumble.left = clampLevel(left->get<double>());
    if (hasRight) rumble.right = clampLevel(right->get<double>());
    return rumble;
}

#ifdef __SWITCH__
// Nonzero output is re-sent so a dropped HID write cannot leave a stale level.
constexpr int64_t kRefreshMs = 250;

struct Binding {
    HidNpadIdType id;
    HidNpadStyleTag style;
    int count;
    const char *name;
};

std::optional<Binding> bindingFor(uint32_t styleSet, int index) {
    if (styleSet & HidNpadStyleTag_NpadHandheld)
        return Binding{HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2, "handheld"};
    if (styleSet & HidNpadStyleTag_NpadFullKey)
        return Binding{HidNpadIdType(index), HidNpadStyleTag_NpadFullKey, 2, "fullkey"};
    if (styleSet & HidNpadStyleTag_NpadJoyDual)
        return Binding{HidNpadIdType(index), HidNpadStyleTag_NpadJoyDual, 2, "joydual"};
    if (styleSet & HidNpadStyleTag_NpadJoyLeft)
        return Binding{HidNpadIdType(index), HidNpadStyleTag_NpadJoyLeft, 1, "joyleft"};
    if (styleSet & HidNpadStyleTag_NpadJoyRight)
        return Binding{HidNpadIdType(index), HidNpadStyleTag_NpadJoyRight, 1, "joyright"};
    return std::nullopt;
}

// Main-thread only.
struct Output {
    HidVibrationDeviceHandle handles[2]{};
    int count = 0;
    uint32_t boundStyle = 0;
    uint32_t failedStyle = 0;
    int64_t retryAt = 0;
    float sentLow = 0, sentHigh = 0;
    int64_t sentAt = 0;
    bool active = false;

    bool send(float low, float high, int64_t now) {
        if (!count) return false;
        HidVibrationValue values[2];
        for (int i = 0; i < count; ++i)
            values[i] = HidVibrationValue{low, 160.f, high, 320.f};
        if (R_FAILED(hidSendVibrationValues(handles, values, count))) {
            count = 0;
            boundStyle = 0;
            return false;
        }
        sentLow = low;
        sentHigh = high;
        sentAt = now;
        active = low > 0 || high > 0;
        return true;
    }

    void silence(int64_t now) {
        if (active) send(0, 0, now);
        active = false;
        sentLow = sentHigh = 0;
    }

    bool bind(uint32_t styleSet, int64_t now, int index) {
        const auto binding = bindingFor(styleSet,index);
        if (!binding) return false;
        const uint32_t style = uint32_t(binding->style);
        if (count && boundStyle == style) return true;
        if (failedStyle == style && now < retryAt) return false;
        silence(now);
        count = 0;
        boundStyle = 0;
        for (int wanted = binding->count; wanted >= 1; --wanted) {
            if (R_SUCCEEDED(hidInitializeVibrationDevices(handles, wanted, binding->id, binding->style))) {
                count = wanted;
                break;
            }
        }
        if (!count) {
            if (failedStyle != style) {
                char line[64];
                std::snprintf(line, sizeof line, "vibration-bind-failed-%s", binding->name);
                diagnostic(line);
            }
            failedStyle = style;
            retryAt = now + 1000;
            return false;
        }
        boundStyle = style;
        failedStyle = 0;
        char line[64];
        std::snprintf(line, sizeof line, "vibration-bind-style-%s-handles-%d", binding->name, count);
        diagnostic(line);
        return true;
    }

    void apply(uint32_t styleSet, float low, float high, int64_t now, int index) {
        const bool wantOn = low > 0 || high > 0;
        if (!wantOn && !active) return;
        const uint32_t previousStyle = boundStyle;
        if (!bind(styleSet, now,index)) return;
        const bool rebound = boundStyle != previousStyle;
        if (!rebound && low == sentLow && high == sentHigh && (!wantOn || now - sentAt < kRefreshMs))
            return;
        if (!send(low, high, now) && bind(styleSet, now,index)) send(low, high, now);
    }
};

std::array<Output,2> outputs;
#endif
} // namespace

std::optional<Rumble> parse(const uint8_t *data, size_t size) noexcept {
    if (!data || size < 2 || size > kMaxMessageBytes || data[0] != '{') return std::nullopt;
    if (std::memchr(data, 0, size)) return std::nullopt;
    const char *text = reinterpret_cast<const char *>(data);
    const char needle[] = "gpd_vibration";
    if (std::search(text, text + size, needle, needle + sizeof needle - 1) == text + size) return std::nullopt;
    try {
        return parseObject(Json::parse(text, text + size, nullptr, false));
    } catch (...) {
        return std::nullopt;
    }
}

float amplitude(int level, int strength) noexcept {
    const float value = std::clamp(level, 0, 255) / 255.f * std::clamp(strength, 0, 100) / 100.f * kMaxAmplitude;
    return std::clamp(value, 0.f, 1.f);
}

Rumble effective(Rumble latest, int64_t lastUpdateMs, int64_t nowMs, bool allowed, int hold) noexcept {
    if (!allowed || lastUpdateMs <= 0 || nowMs - lastUpdateMs > hold) return {};
    return latest;
}

void submit(Rumble rumble) noexcept {
    if (stopped.load(std::memory_order_relaxed)) return;
    const int64_t now = monotonicMs();
    latestPacked[rumble.index].store(uint32_t(std::clamp(rumble.left, 0, 255)) << 8 | uint32_t(std::clamp(rumble.right, 0, 255)),
                       std::memory_order_relaxed);
    const int64_t previous = latestAt[rumble.index].exchange(now, std::memory_order_acq_rel);
    if (sessionMessages.fetch_add(1, std::memory_order_relaxed) == 0) diagnostic("vibration-first-message");
    if (previous > 0) {
        const int64_t interval = now - previous;
        int64_t seen = sessionMaxIntervalMs.load(std::memory_order_relaxed);
        while (interval > seen && !sessionMaxIntervalMs.compare_exchange_weak(seen, interval, std::memory_order_relaxed)) {
        }
    }
}

void setStrength(int percent) noexcept {
    strengthPercent.store(std::clamp(percent, 0, 100), std::memory_order_relaxed);
}

int strength() noexcept {
    return strengthPercent.load(std::memory_order_relaxed);
}

void setHoldMs(int ms) noexcept {
    holdMs.store(std::clamp(ms, kMinHoldMs, kMaxHoldMs), std::memory_order_relaxed);
}

void tick(int64_t nowMs, uint32_t styleSet, bool allowed, int index) noexcept {
    const uint32_t packed = latestPacked[index].load(std::memory_order_relaxed);
    const Rumble latest{int(packed >> 8 & 0xff), int(packed & 0xff)};
    const Rumble target = effective(latest, latestAt[index].load(std::memory_order_acquire), nowMs,
                                    allowed && !stopped.load(std::memory_order_relaxed),
                                    holdMs.load(std::memory_order_relaxed));
    const int percent = strength();
#ifdef __SWITCH__
    auto &output=outputs[index];
    if(!styleSet){output.silence(nowMs);output.count=0;output.boundStyle=0;return;}
    output.apply(styleSet, amplitude(target.left, percent), amplitude(target.right, percent), nowMs,index);
#else
    (void)styleSet;
    (void)target;
    (void)percent;
#endif
}

void stop() noexcept {
#ifdef __SWITCH__
    for(auto &output:outputs)output.silence(monotonicMs());
#endif
}

void endSession() noexcept {
    stop();
    const uint32_t messages = sessionMessages.exchange(0, std::memory_order_relaxed);
    const int64_t maxInterval = sessionMaxIntervalMs.exchange(0, std::memory_order_relaxed);
    for(auto &value:latestAt)value.store(0,std::memory_order_release);
    for(auto &value:latestPacked)value.store(0,std::memory_order_relaxed);
    if (!messages) return;
    char line[96];
    std::snprintf(line, sizeof line, "vibration-session-messages-%u-max-interval-ms-%lld", unsigned(messages),
                  (long long)maxInterval);
    diagnostic(line);
}

void shutdown() noexcept {
    stopped.store(true, std::memory_order_relaxed);
    endSession();
}
} // namespace uu::vibration
