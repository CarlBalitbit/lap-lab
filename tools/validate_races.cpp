#define main simulatorInteractiveMain
#include "../f1_track_sim.cpp"
#undef main

int main(int argc,char* argv[]){
    if(argc!=2)return 2;
    try{
        for(const auto& file:discoverCircuits(argv[1])){
            const auto track=loadTrackFile(file.path);
            for(auto mode:{LapMode::RaceStanding,LapMode::RaceRolling}){
                auto entry=presetEntry(2);
                simulateRace(entry,track,1.0,1.225,.25,mode,true,3,10.0);
                if(entry.raceLaps!=3||entry.lapTimes.size()!=3||entry.raceTrace.empty())throw std::runtime_error("Missing race data");
                if(std::abs(entry.raceStartSpeed-(mode==LapMode::RaceStanding?0:10))>1e-8)throw std::runtime_error("Incorrect starting speed");
                if(std::abs(entry.raceTrace.back().distance-entry.result.totalDistance*3)>1e-5)throw std::runtime_error("Race distance mismatch");
                double sum=0;for(double lap:entry.lapTimes){if(!std::isfinite(lap)||lap<=0)throw std::runtime_error("Invalid lap time");sum+=lap;}
                if(std::abs(sum-entry.raceDuration)>1e-6)throw std::runtime_error("Race time mismatch");
                for(int lap=1;lap<3;++lap){
                    const double d=lap*entry.result.totalDistance;
                    const auto a=sampleTrace(entry.raceTrace,d-.00001),b=sampleTrace(entry.raceTrace,d+.00001);
                    if(a.speed<=0||b.speed<=0||std::abs(a.speed-b.speed)>.001||b.time<=a.time)throw std::runtime_error("Discontinuous lap crossing");
                }
                for(const auto& point:entry.raceTrace)if(!std::isfinite(point.speed)||point.speed<0)throw std::runtime_error("Invalid race speed");
                std::cout<<file.path.filename().string()<<','<<(mode==LapMode::RaceStanding?"standing":"rolling")<<",PASS\n"<<std::flush;
            }
        }
        for(int laps:{0,21}){
            bool rejected=false;try{auto entry=presetEntry(2);simulateRace(entry,presetTrack(1),1,1.225,.25,LapMode::RaceStanding,true,laps,0);}catch(const std::exception&){rejected=true;}
            if(!rejected)throw std::runtime_error("Invalid lap count accepted");
        }
        Track open;open.addStraight(100);bool rejected=false;
        try{auto entry=presetEntry(2);simulateRace(entry,open,1,1.225,.25,LapMode::RaceStanding,true,3,0);}catch(const std::exception&){rejected=true;}
        if(!rejected)throw std::runtime_error("Open race track accepted");
        std::cout<<"PASS: all circuits, 3-lap standing/rolling races, continuous crossings, invalid inputs.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
