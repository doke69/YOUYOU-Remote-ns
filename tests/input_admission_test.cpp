#include "event_queue.hpp"
#include "input_admission.hpp"
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Event {
    std::string type;
    uint64_t generation = 0;
};
struct Inbox {
    std::mutex mutex;
    std::deque<Event> events;
    uu::InputAdmission admission;
    uint64_t generation = 1;
};
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        Inbox inbox;
        int directOldReleases = 0;
        auto enqueue = [&](const std::string &type) {
            return uu::tryEnqueuePeerInput(inbox.mutex, inbox.admission, inbox.events,
                                           Event{type, 0});
        };
        auto postControl = [&](const std::string &type) {
            std::lock_guard lock(inbox.mutex);
            inbox.events.push_back(Event{type, 0});
        };

        require(!enqueue("input"), "input before admission must be dropped");
        uu::openInputAdmission(inbox.admission, inbox.generation);
        require(enqueue("input"), "connected input should enqueue");
        require(enqueue("text-input"), "connected text should enqueue");
        require(enqueue("text-input-cancel"), "keyboard cancellation belongs to the same peer");
        require(enqueue("system-action"), "connected system action should enqueue");
        require(!uu::isPeerInputCommand("connect") && !uu::isPeerInputCommand("login") &&
                    !uu::isPeerInputCommand("refresh") && uu::isPeerInputCommand("text-input-cancel"),
                "local/control commands must not be gated");

        // Disconnect: close admission, direct release old keys, advance peer
        // generation, then reconnect/open a new generation.
        {
            std::lock_guard lock(inbox.mutex);
            uu::closeInputAdmission(inbox.admission);
        }
        ++directOldReleases;
        ++inbox.generation;
        require(!enqueue("input"), "input after close must be dropped");
        uu::openInputAdmission(inbox.admission, inbox.generation);
        require(enqueue("input"), "new-generation input should enqueue");
        postControl("connect");

        std::vector<std::string> handled;
        uu::drainEventBatch(inbox.events, inbox.mutex, [] { return false; },
                            [&](const Event &event) {
            if (event.generation && event.generation != inbox.generation) return;
            handled.push_back(event.type);
        });

        // Old queued input/text/system were popped but generation-filtered.
        // New input and the local connect event (generation 0) are handled.
        require(handled.size() == 2, "only new input and local connect should handle");
        require(handled[0] == "input" && handled[1] == "connect", "drain order/generation filter");
        require(directOldReleases == 1, "old direct release must remain");

        // The queue is a single-consumer FIFO; no purge is performed. A stale
        // event is consumed once and does not poison later batches.
        require(inbox.events.empty(), "drain must consume the fixed batch");
        require(enqueue("release"), "new generation release should enqueue");
        handled.clear();
        uu::drainEventBatch(inbox.events, inbox.mutex, [] { return false; },
                            [&](const Event &event) {
            if (event.generation && event.generation != inbox.generation) return;
            handled.push_back(event.type);
        });
        require(handled.size() == 1 && handled[0] == "release", "subsequent batch");
        std::cout << "PASS: production input admission/enqueue and drain generation filter; "
                     "old direct release preserved; no purge\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
