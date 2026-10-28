#include "ravava/Recorder.h"
#include "ravava/Player.h"
#include <mutex>
#include <random>
#include <thread>
#include <chrono>
#include <cstdio>
#include <deque>

using namespace ravava;

int main(){
    AudioConfig cfg; cfg.frameMs=20; cfg.opusBitrate=16000; cfg.expectedLossPerc=20; cfg.useDtx=false;
    Recorder rec; Player play;
    std::mt19937 rng(1); std::mutex mx; int sent=0, dropped=0, swapped=0;
    std::deque<Bytes> held;
    rec.SetPacketHandler([&](const Bytes &d){
        std::lock_guard<std::mutex> lk(mx); sent++;
        if(rng()%10==0){ dropped++; return; }               // 10% loss
        if(rng()%15==0){ held.push_back(d); swapped++; return; } // reorder: deliver after next
        play.Play(d);
        while(!held.empty()){ play.Play(held.front()); held.pop_front(); }
    });
    rec.SetConfig(&cfg); play.SetConfig(&cfg);
    rec.Start();
    std::this_thread::sleep_for(std::chrono::seconds(4));
    rec.Stop();
    JitterBuffer::Stats st; int depth, ms; play.GetStats(st, depth, ms);
    std::printf("sent=%d dropped=%d reordered_sent=%d\n", sent, dropped, swapped);
    std::printf("recv=%u played=%u lost=%u fec=%u plc=%u late=%u reordered=%u underrun=%u depth=%d\n",
        st.received, st.played, st.lost, st.fec, st.plc, st.late, st.reordered, st.underrun, depth);
    return 0;
}
