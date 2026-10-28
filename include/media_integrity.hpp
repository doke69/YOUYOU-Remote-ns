#pragma once
#include <cstdint>

namespace uu {

// A decoded result may be published/displayed only while both the session
// generation and the same-session recovery sequence still match the values
// captured when its access unit was dequeued.
inline bool mediaPublishAllowed(uint64_t expectedGeneration,uint64_t currentGeneration,
                                uint64_t expectedRecovery,uint64_t currentRecovery) noexcept {
    return expectedGeneration==currentGeneration && expectedRecovery==currentRecovery;
}

enum class ReceiveDisposition {
    Frame,
    Again,
    Eof,
    Error,
};

// Callers pass the library's AVERROR(EAGAIN)/AVERROR_EOF values so this
// classification stays independent of a particular FFmpeg header in tests.
inline ReceiveDisposition classifyReceiveResult(int result,int againCode,int eofCode) noexcept {
    if(result==0)return ReceiveDisposition::Frame;
    if(result==againCode)return ReceiveDisposition::Again;
    if(result==eofCode)return ReceiveDisposition::Eof;
    return ReceiveDisposition::Error;
}

} // namespace uu
