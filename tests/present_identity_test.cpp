#include "present_identity.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool value) {
    if(!value)throw std::runtime_error("present identity mismatch");
}

int main() {
    try {
        uu::PresentIdentity identity;
        // Accepted frame skipped: no serial, then the same serial is submitted.
        require(!identity.gpuSubmitted(0));
        require(identity.gpuSubmitted(7));
        require(!identity.gpuSubmitted(7));
        // No video / reset.
        identity.reset();
        require(!identity.gpuSubmitted(0));
        require(identity.gpuSubmitted(7));
        // SDL: upload without successful draw does not consume identity.
        identity.reset();
        require(!identity.sdlPresented(true,0));
        require(!identity.sdlPresented(false,7));
        require(identity.sdlPresented(true,7));
        require(!identity.sdlPresented(true,7));
        // A failed new upload leaves the old identity; a later successful draw
        // of a new identity is counted once.
        require(identity.sdlPresented(true,8));
        require(!identity.sdlPresented(true,8));
        require(identity.sdlPresented(true,9));
        identity.reset();
        require(identity.sdlPresented(true,9));
        require(!identity.sdlPresented(true,9));
        std::cout<<"PASS: GPU/SDL presentation identity; skip/repeat/reset boundaries\n";
    } catch(const std::exception &e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
