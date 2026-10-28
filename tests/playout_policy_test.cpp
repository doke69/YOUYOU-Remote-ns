#include "playout_policy.hpp"
#include "refresh_pacer.hpp"
#include <cassert>
#include <cmath>
#include <deque>
#include <iostream>
#include <vector>

struct Frame { int64_t dueMs; int id; };
int main() {
    std::deque<Frame> q;
    assert(!uu::choosePlayout(q,100).available);
    q={{108,1},{110,2}};
    assert(!uu::choosePlayout(q,107).available);
    auto d=uu::choosePlayout(q,116);
    assert(d.available && d.discard==0); // burst must not erase frame 1
    q.pop_front();
    d=uu::choosePlayout(q,133);
    assert(d.available && d.discard==0);
    q={{100,1},{116,2},{132,3},{220,4}};
    d=uu::choosePlayout(q,200);
    assert(d.available && d.discard==2); // recover from stall, do not select future
    assert(!uu::choosePlayout(std::deque<Frame>{{300,1}},200).available);
    q={{100,1},{116,2}};
    assert(uu::choosePlayout(q,150).discard==0); // boundary is strictly >50ms

    // Replay a 60-fps source delivered as alternating close/distant pairs.
    // Every frame is ready by its output tick. The newest-due policy drops
    // one frame and repeats on the next tick; FIFO must retain all frames.
    auto replay=[](bool fifo) {
        std::deque<Frame> frames;
        std::vector<int> selected;
        int next=0;
        for(int tick=0;tick<122;++tick) {
            const int64_t now=17+int64_t(tick)*1000/60;
            while(next<120) {
                const int64_t arrival=int64_t(next/2)*1000/30+(next%2?1:0);
                if(arrival>now) break;
                frames.push_back({arrival+8,next++});
            }
            auto choice=uu::choosePlayout(frames,now);
            if(!choice.available) continue;
            if(!fifo) while(choice.discard+1<frames.size() && frames[choice.discard+1].dueMs<=now) ++choice.discard;
            for(std::size_t i=0;i<choice.discard;++i) frames.pop_front();
            selected.push_back(frames.front().id);frames.pop_front();
        }
        return selected;
    };
    const auto fifo=replay(true), newest=replay(false);
    assert(fifo.size()==120 && newest.size()<fifo.size());
    for(int i=0;i<120;++i) assert(fifo[i]==i);

    // Recovery drops only enough to fit the budget, not every eligible frame.
    q={{100,1},{116,2},{132,3},{148,4}};
    d=uu::choosePlayout(q,140,17);
    assert(d.available && d.discard==2); // preserve frame 4 for its future tick
    d=uu::choosePlayout(q,133,17);
    assert(d.discard==1); // do not skip the still-usable frame due at 116
    assert(uu::choosePlayout(std::deque<Frame>{{8,1}},7,17).available==false);
    assert(uu::choosePlayout(std::deque<Frame>{{8,1}},8,17).available);
    assert(uu::choosePlayout(std::deque<Frame>{{0,1}},0,17).available);

    // Actual production selector: requested delay, 30/60 Hz arrival jitter,
    // no fabricated depth reserve, bounded stale recovery after a UI stall.
    for(int hz:{30,60}) for(int delay:{0,8,16,35,100}) {
        std::deque<Frame> frames;
        int next=0,chosen=0,discarded=0;
        int64_t maxAge=0;
        const int count=hz*10;
        for(int tick=0;tick<620;++tick) {
            const int64_t now=17+int64_t(tick)*1000/60;
            while(next<count) {
                const int64_t arrival=int64_t(next)*1000/hz+(next*37%7)-3;
                if(arrival>now)break;
                frames.push_back({arrival+delay,next++});
            }
            auto choice=uu::choosePlayout(frames,now,(1000+hz-1)/hz+17);
            if(!choice.available)continue;
            discarded+=int(choice.discard);
            for(size_t i=0;i<choice.discard;++i)frames.pop_front();
            maxAge=std::max(maxAge,now-(frames.front().dueMs-delay));
            frames.pop_front();++chosen;
        }
        assert(chosen==count && discarded==0);
        // Poll phase is 17 ms, with up to 3 ms early/late arrivals. A burst
        // can carry that phase error into the next FIFO frame without a drop.
        std::cout<<"replay hz="<<hz<<" delay="<<delay<<" selected="<<chosen
                 <<" discarded="<<discarded<<" max-age="<<maxAge<<'\n';
        assert(maxAge>=delay && maxAge<=delay+24);
    }
    q={{100,1},{116,2},{132,3},{148,4},{164,5},{180,6},{196,7},{212,8}};
    d=uu::choosePlayout(q,210,17);
    assert(d.available && d.discard==6); // only one recovery, leaves future intact
    q.clear(); // generation/reset discards retained frames and no scheduling credit survives
    assert(!uu::choosePlayout(q,10000,17).available);
    q={{10008,9}};
    assert(!uu::choosePlayout(q,10007,17).available);
    assert(uu::choosePlayout(q,10008,17).available);

    // Absolute pacing must not accumulate a 17ms millisecond rounding drift.
    // Exercise nonblocking present, hardware-blocking present, and a long stall.
    uu::RefreshPacer pacer;
    double now=0;
    for(int i=0;i<600;++i) {
        const double finish=now+2000;
        now=finish+pacer.remaining(now,finish);
        pacer.advance(now);
    }
    assert(std::abs(now-10000000.0)<0.01);
    pacer.reset();now=0;
    for(int i=0;i<600;++i) {
        const double finish=now+1000000.0/60.0;
        const double wait=pacer.remaining(now,finish);
        assert(wait<0.01); // real vsync must not be followed by another full wait
        now=finish+wait;pacer.advance(now);
    }
    now+=1000000;
    const double finish=now+2000;
    const double wait=pacer.remaining(now,finish);
    assert(wait>14000 && wait<17000);
    pacer.advance(finish+wait);
    const double following=finish+wait;
    assert(pacer.remaining(following,following+2000)>14000);
    pacer.reset();
    assert(pacer.remaining(0,2000)>14000);

    // Non-streaming menu software cap: 2ms UI work leaves only the remaining
    // budget; 120 ticks stay on the 60Hz grid.
    pacer.reset();now=0;
    for(int i=0;i<120;++i) {
        const double finish=now+2000;
        const double wait=pacer.remaining(now,finish);
        assert(wait>14600 && wait<14700);
        now=finish+wait;pacer.advance(now);
    }
    assert(std::abs(now-2000000.0)<0.01);
    // A hardware-blocked present consumes the budget; the next menu tick only
    // waits the remainder rather than a second fixed sleep.
    pacer.reset();now=0;
    const double hardwareFinish=1000000.0/60.0;
    assert(pacer.remaining(now,hardwareFinish)<0.01);
    now=hardwareFinish;pacer.advance(now);
    const double menuFinish=now+2000;
    const double menuWait=pacer.remaining(now,menuFinish);
    assert(menuWait>14600 && menuWait<14700);
    pacer.advance(menuFinish+menuWait);
    // A long elapsed segment is not repaid as a burst.
    pacer.reset();now=0;
    const double slowWait=pacer.remaining(now,10000);
    assert(slowWait>6600 && slowWait<6700);
    pacer.advance(10000+slowWait);
    assert(pacer.remaining(10000+slowWait,12000+slowWait)>14600);

    std::cout<<"PASS: FIFO burst replay "<<fifo.size()<<"/120 vs newest "<<newest.size()
             <<"/120; eligibility, stall recovery, 600-tick software/hardware pacing, menu/switch budget, reset\n";
}
