#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

// Host rumble forwarding: CONTROL_DATA_CHANNEL `gpd_vibration` -> HD rumble.
// submit() is called from the RTC thread; every HID call happens in tick()/stop()
// on the main thread.
namespace uu::vibration {
struct Rumble {
    int left = 0, right = 0;
    int index = 0;
    bool operator==(const Rumble &other) const { return left == other.left && right == other.right && index == other.index; }
    bool operator!=(const Rumble &other) const { return !(*this == other); }
};

constexpr int kDefaultStrength = 60;
constexpr int kDefaultHoldMs = 300;
constexpr int kMinHoldMs = 100, kMaxHoldMs = 2000;
constexpr size_t kMaxMessageBytes = 512;

// Accepts only {"action":"gpd_vibration","index":0 or 1,...}; motors are clamped to 0..255.
std::optional<Rumble> parse(const uint8_t *data, size_t size) noexcept;
// level/255 * strength/100 * 0.8, clamped to 0..1.
float amplitude(int level, int strength) noexcept;
// Motor levels to output now: zero when gated or when the host stopped refreshing.
Rumble effective(Rumble latest, int64_t lastUpdateMs, int64_t nowMs, bool allowed, int holdMs) noexcept;

void submit(Rumble rumble) noexcept;
void setStrength(int percent) noexcept;
int strength() noexcept;
void setHoldMs(int ms) noexcept;
// styleSet is padGetStyleSet() of the main PadState.
void tick(int64_t nowMs, uint32_t styleSet, bool allowed, int index=0) noexcept;
// Silences the motors now; later ticks may resume output.
void stop() noexcept;
// Logs the session summary and clears the last host value.
void endSession() noexcept;
// Final stop before RTC teardown; later submits are ignored.
void shutdown() noexcept;
} // namespace uu::vibration
