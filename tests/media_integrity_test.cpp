#include "media_integrity.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool value) {
    if(!value)throw std::runtime_error("media integrity mismatch");
}

int main() {
    try {
        // Same generation/recovery is publishable.
        require(uu::mediaPublishAllowed(7,7,11,11));
        // Cross-session invalidation.
        require(!uu::mediaPublishAllowed(7,8,11,11));
        // Same-session overflow/recovery invalidates an in-flight AU.
        require(!uu::mediaPublishAllowed(7,7,11,12));
        // An AU dequeued after the recovery boundary remains publishable.
        require(uu::mediaPublishAllowed(7,7,12,12));
        // A second recovery invalidates the first recovery's AU again.
        require(!uu::mediaPublishAllowed(7,7,12,13));

        require(uu::classifyReceiveResult(0,-11,-541478725)==uu::ReceiveDisposition::Frame);
        require(uu::classifyReceiveResult(-11,-11,-541478725)==uu::ReceiveDisposition::Again);
        require(uu::classifyReceiveResult(-541478725,-11,-541478725)==uu::ReceiveDisposition::Eof);
        require(uu::classifyReceiveResult(-5,-11,-541478725)==uu::ReceiveDisposition::Error);
        std::cout<<"PASS: generation/recovery publish gate; receive EAGAIN/EOF/error classification\n";
    } catch(const std::exception &e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
