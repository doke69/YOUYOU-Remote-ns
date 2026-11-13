#include "config_store.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#ifdef NDEBUG
#undef NDEBUG
#endif
// Keep assertions enabled in the Release test executable.
#include <cassert>

int main() {
    using namespace std::chrono_literals;
    const uu::QualityConfigValues a{1280,720,60,6,20,8,true,true};
    auto b=a;b.cap=30;
    auto c=a;c.cap=40;
    // Suppression is eligible only when the custom cap is the sole change.
    assert(b.isBitrateOnlyChangeFrom(a) && a.isBitrateOnlyChangeFrom(b));
    assert(!a.isBitrateOnlyChangeFrom(a));
    for(int field=0;field<8;++field) {
        auto mixed=b;
        switch(field) {
            case 0:mixed.width=1920;break;
            case 1:mixed.height=1080;break;
            case 2:mixed.fps=30;break;
            case 3:mixed.quality=3;break;
            case 4:mixed.delay=16;break;
            case 5:mixed.vsync=false;break;
            case 6:mixed.hevc=false;break;
            case 7:break;
        }
        auto previous=a;
        if(field==7)previous.quality=3;
        assert(!mixed.isBitrateOnlyChangeFrom(previous));
    }
    std::mutex mutex;std::condition_variable cv;
    bool entered=false,release=false;
    std::vector<int> written;
    uu::QualityConfigWriter writer([&](const auto &q){
        std::unique_lock lock(mutex);
        if(q.cap==20){entered=true;cv.notify_one();cv.wait(lock,[&]{return release;});}
        written.push_back(q.cap);
    });
    assert(writer.submit(1,a));
    {std::unique_lock lock(mutex);assert(cv.wait_for(lock,2s,[&]{return entered;}));}
    // Submission and status reads must finish while storage is still blocked.
    assert(writer.submit(2,b));assert(writer.submit(3,c));
    assert(writer.snapshot().completed==0 && writer.stats().replaced==1);
    assert(!writer.submit(2,b));
    {std::lock_guard lock(mutex);release=true;}cv.notify_one();
    writer.shutdown();
    assert((written==std::vector<int>{20,40}));
    assert(writer.snapshot().order==3 && writer.snapshot().completed==2 && !writer.snapshot().error);
    assert(writer.stats().retainedBytes==0 && !writer.submit(4,a));

    // Disk failures are visible and do not kill subsequent persistence.
    uu::QualityConfigWriter failed([](const auto &){throw std::runtime_error("injected save error");});
    assert(failed.submit(1,a));failed.shutdown();
    assert(failed.snapshot().order==1 && failed.snapshot().failures==1 && failed.snapshot().error);
    uu::QualityConfigWriter recovering([&](const auto &q){
        if(q.cap==20){std::lock_guard lock(mutex);entered=true;cv.notify_one();throw std::runtime_error("injected");}
        written.push_back(q.cap);
    });
    entered=false;assert(recovering.submit(1,a));
    {std::unique_lock lock(mutex);assert(cv.wait_for(lock,2s,[&]{return entered;}));}
    assert(recovering.submit(2,b));recovering.shutdown();
    assert(recovering.snapshot().order==2 && recovering.snapshot().failures==1 && !recovering.snapshot().error);

    // Exercise production file persistence and destructor flushing in an isolated directory.
    namespace fs=std::filesystem;
    const auto original=fs::current_path();
    const auto temporary=original/("quality-writer-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(temporary);fs::current_path(temporary);
    {std::ofstream file("config.ini");file<<"frame_pacing=0 # keep\nunknown_key=retained\n";}
    {
        uu::QualityConfigWriter real;
        assert(real.submit(1,a));assert(real.submit(2,b));
    }
    uu::ConfigStore::instance().reload();
    assert(uu::ConfigStore::instance().value("max_bitrate_mbps")=="30");
    assert(uu::ConfigStore::instance().value("frame_pacing")=="0");
    assert(uu::ConfigStore::instance().value("unknown_key")=="retained");
    fs::current_path(original);fs::remove_all(temporary);
}
