#include "runtime/latest_snapshot_worker.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace std::chrono_literals;
static void check(bool v) {if(!v) throw std::runtime_error("snapshot worker regression");}
struct Job {
    std::uint64_t order;
    std::size_t bytes;
    std::shared_ptr<int> payload;
    std::size_t retainedBytes() const noexcept {return bytes;}
};
int main() {
    std::mutex gate;std::condition_variable changed;
    bool entered=false,release=false;
    std::vector<std::uint64_t> written;
    uu::runtime::LatestSnapshotWorker<Job> worker(100,[&](const Job &job){
        std::unique_lock lock(gate);
        if(job.order==1) {entered=true;changed.notify_one();changed.wait(lock,[&]{return release;});}
        written.push_back(job.order);
        if(job.order==200) throw std::runtime_error("synthetic writer failure");
    });
    check(worker.submit({1,40,{}}));
    {std::unique_lock lock(gate);check(changed.wait_for(lock,2s,[&]{return entered;}));}
    auto old=std::make_shared<int>(2);std::weak_ptr<int> oldRef=old;
    check(worker.submit({2,40,std::move(old)}));
    check(worker.submit({3,60,{}}));check(oldRef.expired());
    auto stats=worker.stats();check(stats.retainedBytes==100 && stats.replaced==1);
    check(!worker.submit({4,61,{}})); // includes the in-flight job's 40 bytes
    check(!worker.submit({2,1,{}})); // cannot publish an old snapshot after newer
    std::weak_ptr<int> previous;
    for(std::uint64_t order=10;order<110;++order) {
        auto payload=std::make_shared<int>(int(order));
        check(worker.submit({order,60,payload}));
        check(previous.expired());previous=payload;
    }
    check(worker.stats().replaced==101 && worker.stats().retainedBytes==100);
    check(!worker.drainFor(10ms));
    {std::lock_guard lock(gate);release=true;}changed.notify_one();
    check(worker.drainFor(2s));
    check(written==std::vector<std::uint64_t>({1,109}) && previous.expired());
    check(worker.submit({200,20,{}}));check(worker.drainFor(2s));
    check(worker.stats().failures==1);
    check(worker.submit({201,20,{}}));check(worker.drainFor(2s));
    worker.shutdown();worker.shutdown();check(!worker.submit({202,1,{}}));
    stats=worker.stats();check(stats.completed==4 && stats.peakBytes<=100 && stats.retainedBytes==0);
    // An empty byte charge still represents a running writer and must be joined.
    entered=false;release=false;
    uu::runtime::LatestSnapshotWorker<Job> zero(0,[&](const Job &){
        std::unique_lock lock(gate);entered=true;changed.notify_one();changed.wait(lock,[&]{return release;});
    });
    check(zero.submit({1,0,{}}));
    {std::unique_lock lock(gate);check(changed.wait_for(lock,2s,[&]{return entered;}));}
    check(!zero.drainFor(10ms));
    {std::lock_guard lock(gate);release=true;}changed.notify_one();zero.shutdown();
    std::cout<<"PASS: blocked writer, pending replacement/release, combined byte budget, stale order, timeout, exceptions, zero-byte running and shutdown\n";
}
