#define main simulatorInteractiveMain
#include "../f1_track_sim.cpp"
#undef main

namespace convergenceValidation {
// Accuracy targets are diagnostic; mathematical references and invariants block CI.
constexpr std::array<double,4> steps{{0.5,0.25,0.125,0.0625}};
int failures=0;
int diagnosticExceedances=0;

void require(bool condition,const std::string& reason)
{
    if(!condition)throw std::runtime_error(reason);
}

bool accuracyExceeded(double error,double limit)
{
    require(std::isfinite(error)&&error>=0&&std::isfinite(limit)&&limit>=0,
            "Nonfinite or invalid diagnostic comparison");
    const bool exceeded=error>limit;
    if(exceeded)++diagnosticExceedances;
    return exceeded;
}

void diagnosticTarget(const std::string& label,double error,double limit)
{
    const bool exceeded=accuracyExceeded(error,limit);
    std::cout<<(exceeded?"DIAGNOSTIC_TARGET_EXCEEDED,":"DIAGNOSTIC_WITHIN_TARGET,")
             <<label<<",error="<<error<<",unchanged_target="<<limit<<",blocking=false\n";
}

void target(const std::string& label,double error,double limit)
{
    const bool pass=std::isfinite(error)&&error>=0&&std::isfinite(limit)&&limit>=0&&error<=limit;
    if(!pass)++failures;
    std::cout<<(pass?"MANDATORY_PASS,":"MANDATORY_FAIL,")<<label<<",error="<<error<<",acceptance_target="<<limit<<'\n';
}

void validSimulation(const Car& car,const Simulation& result,bool periodic,double mu=1)
{
    require(!result.cells.empty()&&result.speed.size()==result.cells.size()+1,"Invalid cell/speed dimensions");
    require(std::isfinite(result.totalDistance)&&result.totalDistance>0&&std::isfinite(result.totalTime)&&result.totalTime>0,
            "Invalid simulation totals");
    for(double speed:result.speed)require(std::isfinite(speed)&&speed>=0,"Invalid simulation speed");
    for(double boundary:result.sectorBounds)require(std::isfinite(boundary),"Invalid sector boundary");
    require(result.sectorBounds[0]==0&&result.sectorBounds[1]>0&&result.sectorBounds[2]>result.sectorBounds[1]
            &&result.sectorBounds[3]>result.sectorBounds[2]
            &&std::abs(result.sectorBounds[3]-result.totalDistance)<=1e-5,"Broken sector ordering/coverage");
    double distance=0,time=0;
    for(std::size_t i=0;i<result.cells.size();++i){
        const auto& c=result.cells[i];
        require(std::isfinite(c.length)&&c.length>0&&std::isfinite(c.curvature)&&std::isfinite(c.grade)
                &&std::isfinite(c.elevation)&&std::isfinite(c.density)&&c.density>0,"Invalid physics cell");
        const double a=result.speed[i],b=result.speed[i+1];
        require(a+b>0,"Zero-speed travel cell");
        const double acceleration=(b*b-a*a)/(2*c.length);
        const double braking=brakeBound(car,mu,1.225,c,a,b),driving=driveBound(car,mu,1.225,c,a,b);
        require(std::isfinite(acceleration)&&std::isfinite(braking)&&std::isfinite(driving)
                &&acceleration>=-braking-1e-6&&acceleration<=driving+1e-6,"Infeasible cell acceleration");
        const double fastest=std::max(a,b);
        for(double q:{0.45,0.55})require(q*car.mass*fastest*fastest*std::abs(c.curvature)
                <=car.axleCapacity(mu,c.density,fastest,q,c.grade)+1e-6,"Infeasible lateral demand");
        distance+=c.length;time+=2*c.length/(a+b);
    }
    require(std::isfinite(distance)&&std::abs(distance-result.totalDistance)<=1e-5,"Broken total distance");
    require(std::isfinite(time)&&std::abs(time-result.totalTime)<=1e-6+1e-10*result.totalTime,"Broken total time");
    const auto sectors=sectorTimes(result);
    double sum=0;for(double t:sectors){require(std::isfinite(t)&&t>0,"Invalid sector time");sum+=t;}
    require(std::abs(sum-result.totalTime)<=1e-6+1e-10*result.totalTime,"Broken sector sum");
    if(periodic)require(result.closed&&std::abs(result.speed.front()-result.speed.back())<=1e-8,"Broken qualifying closure");
}

void validTrace(const std::vector<TracePoint>& trace,double distance,double time)
{
    require(trace.size()>=2,"Empty/short trace");
    require(trace.front().distance==0&&trace.front().time==0,"Invalid trace origin");
    for(std::size_t i=0;i<trace.size();++i){
        const auto& p=trace[i];
        require(std::isfinite(p.speed)&&p.speed>=0&&std::isfinite(p.time)&&p.time>=0
                &&std::isfinite(p.distance)&&p.distance>=0,"Nonfinite/invalid trace point");
        if(i)require(p.distance>trace[i-1].distance&&p.time>trace[i-1].time,"Nonmonotonic trace");
    }
    require(std::abs(trace.back().distance-distance)<=1e-5
            &&std::abs(trace.back().time-time)<=1e-6+1e-10*time,"Broken trace endpoint");
}

TracePoint checkedSample(const std::vector<TracePoint>& trace,double distance)
{
    const auto p=sampleTrace(trace,distance);
    require(std::isfinite(p.speed)&&p.speed>=0&&std::isfinite(p.time)&&p.time>=0
            &&std::isfinite(p.distance)&&std::abs(p.distance-distance)<=1e-5,"Invalid interpolated output");
    return p;
}

void refinement(const std::string& label,const std::array<double,4>& values)
{
    const double coarse=std::abs(values[0]-values[1]),fine=std::abs(values[1]-values[2]);
    const double floor=1e-10*std::max(1.0,std::abs(values[2]));
    std::cout<<"REFINEMENT,"<<label<<",value_0.5="<<values[0]<<",value_0.25="<<values[1]<<",value_0.125="<<values[2]
             <<",error_0.5_vs_fine="<<std::abs(values[0]-values[2])<<",error_0.25_vs_fine="<<fine<<",error_0.125_vs_fine=0"
             <<",delta_0.5_0.25="<<coarse<<",delta_0.25_0.125="<<fine;
    if(coarse>floor&&fine>floor)std::cout<<",ratio="<<coarse/fine<<",observed_order="<<std::log2(coarse/fine);
    else std::cout<<",ratio=NA (near numerical floor)";
    std::cout<<",interpretation=grid stability; no exact reference\n";
    const double extra=std::abs(values[2]-values[3]);
    std::cout<<"EXTRA_REFINEMENT,"<<label<<",value_0.25="<<values[1]<<",value_0.125="<<values[2]
             <<",value_0.0625="<<values[3]<<",delta_0.25_0.125="<<fine<<",delta_0.125_0.0625="<<extra;
    if(fine>floor&&extra>floor)std::cout<<",ratio="<<fine/extra<<",observed_order="<<std::log2(fine/extra);
    else std::cout<<",ratio=NA (near numerical floor)";
    std::cout<<",reference=grid differences; not exact errors\n";
}

void analyticalStraight()
{
    Car car;car.mass=800;car.power=80;car.frontalArea=1.5;
    Track track;track.addStraight(1000);
    const double exactSpeed=std::cbrt(20.0*20*20+3*80000.0*1000/800);
    const double exactTime=800.0*(exactSpeed*exactSpeed-400)/(2*80000);
    // Independent traction check: no aero, fixed normal load; P/v decreases.
    const double tireLoad=0.55*800*9.81/2;
    const double rearCapacity=2*10*tireLoad*std::pow(2000/tireLoad,0.1);
    if(rearCapacity<=80000.0/20)throw std::runtime_error("Analytical fixture is traction limited");
    std::array<double,4> speedErrors{},timeErrors{};
    for(std::size_t i=0;i<steps.size();++i){
        const auto result=simulateTrack(car,track,10,1.225,20,steps[i],LapMode::SingleRun,false);
        validSimulation(car,result,false,10);
        validTrace(makeTrace(result),1000,result.totalTime);
        require(std::isfinite(result.totalTime)&&result.totalTime>0,"Invalid exact-fixture time");
        speedErrors[i]=std::abs(result.speed.back()-exactSpeed);
        timeErrors[i]=std::abs(result.totalTime-exactTime);
        std::cout<<"EXACT,power_straight,step="<<steps[i]<<",cells="<<result.cells.size()
                 <<",speed="<<result.speed.back()<<",expected_speed="<<exactSpeed<<",speed_error="<<speedErrors[i]
                 <<",speed_error_percent="<<100*speedErrors[i]/exactSpeed<<",time="<<result.totalTime
                 <<",expected_time="<<exactTime<<",time_error="<<timeErrors[i]<<",time_error_percent="<<100*timeErrors[i]/exactTime<<'\n';
    }
    for(const auto& item : {std::make_pair("speed",speedErrors),std::make_pair("time",timeErrors)}){
        const double floor=1e-10;
        for(int i=1;i<4;++i)target(std::string("exact error decreases,")+item.first,item.second[i],item.second[i-1]+floor);
        std::cout<<"EXACT_REFINEMENT,"<<item.first;
        for(int i=0;i<3;++i){
            if(item.second[i]>floor&&item.second[i+1]>floor)std::cout<<",ratio_"<<i<<'='<<item.second[i]/item.second[i+1]<<",order_"<<i<<'='<<std::log2(item.second[i]/item.second[i+1]);
            else std::cout<<",ratio_"<<i<<"=NA";
        }
        std::cout<<",interpretation=convergence toward independent exact solution\n";
    }
    target("exact finest speed",speedErrors[2],0.001*exactSpeed);
    target("exact finest time",timeErrors[2],0.001*exactTime);
    target("exact 0.0625 speed",speedErrors[3],0.001*exactSpeed);
    target("exact 0.0625 time",timeErrors[3],0.001*exactTime);
}

void selfTest()
{
    const auto car=presetEntry(2).car;
    const auto baseline=simulateTrack(car,presetTrack(2),1,1.225,0,0.5,LapMode::Qualifying,true);
    validSimulation(car,baseline,true);
    const auto trace=makeTrace(baseline);
    validTrace(trace,baseline.totalDistance,baseline.totalTime);
    auto rejected=[](const char* name,const auto& work){
        try{work();}catch(const std::exception&){std::cout<<"MANDATORY_SELF_TEST_PASS,"<<name<<'\n';return;}
        ++failures;std::cout<<"MANDATORY_SELF_TEST_FAIL,"<<name<<",invalid output accepted\n";
    };
    rejected("nonfinite diagnostic",[]{accuracyExceeded(std::numeric_limits<double>::quiet_NaN(),1);});
    rejected("speed dimensions",[&]{auto bad=baseline;bad.speed.pop_back();validSimulation(car,bad,true);});
    rejected("nonfinite speed",[&]{auto bad=baseline;bad.speed[0]=std::numeric_limits<double>::infinity();validSimulation(car,bad,true);});
    rejected("invalid cell",[&]{auto bad=baseline;bad.cells[0].length=0;validSimulation(car,bad,true);});
    rejected("broken total time",[&]{auto bad=baseline;bad.totalTime+=1;validSimulation(car,bad,true);});
    rejected("nonmonotonic trace",[&]{auto bad=trace;bad[1].time=0;validTrace(bad,baseline.totalDistance,baseline.totalTime);});
    rejected("broken trace endpoint",[&]{validTrace(trace,baseline.totalDistance+1,baseline.totalTime);});
    rejected("invalid sector coverage",[&]{auto bad=baseline;bad.sectorBounds[2]=bad.sectorBounds[1];validSimulation(car,bad,true);});
}

// Dense provisional accuracy exceedances are nonblocking; invalid results throw.
// Every point is printed, including every target exceedance on either grid pair.
void denseMonza(const std::array<std::vector<TracePoint>,4>& traces)
{
    const auto start=std::chrono::steady_clock::now();
    std::vector<double> distances;
    for(int i=0;i<=1400;++i)distances.push_back(580+i*0.05);
    distances.push_back(608.265); // Preserve the precise original failing location.
    std::sort(distances.begin(),distances.end());
    std::array<int,2> exceeded{};
    std::array<double,2> maxError{},maxDistance{};
    std::array<double,2> bandStart{{-1,-1}},lastExceeded{};
    for(double distance:distances){
        std::array<double,3> v{};
        for(int i=0;i<3;++i)v[i]=checkedSample(traces[i+1],distance).speed;
        std::array<double,2> errors{{std::abs(v[0]-v[1]),std::abs(v[1]-v[2])}};
        std::array<double,2> limits{{0.02+0.002*std::abs(v[1]),0.02+0.002*std::abs(v[2])}};
        std::cout<<"DENSE_MONZA,distance="<<distance<<",v_0.25="<<v[0]<<",v_0.125="<<v[1]<<",v_0.0625="<<v[2]
                 <<",delta_0.25_0.125="<<errors[0]<<",target_0.25_0.125="<<limits[0]
                 <<",delta_0.125_0.0625="<<errors[1]<<",target_0.125_0.0625="<<limits[1];
        if(errors[0]>1e-10&&errors[1]>1e-10)std::cout<<",ratio="<<errors[0]/errors[1];else std::cout<<",ratio=NA";
        for(int pair=0;pair<2;++pair){
            const bool over=accuracyExceeded(errors[pair],limits[pair]);
            std::cout<<",pair_"<<pair<<'='<<(over?"TARGET_EXCEEDED":"WITHIN_TARGET");
            if(errors[pair]>maxError[pair]){maxError[pair]=errors[pair];maxDistance[pair]=distance;}
            if(over){++exceeded[pair];if(bandStart[pair]<0)bandStart[pair]=distance;lastExceeded[pair]=distance;}
        }
        std::cout<<",classification=diagnostic\n";
        for(int pair=0;pair<2;++pair)if(bandStart[pair]>=0&&errors[pair]<=limits[pair]){
            std::cout<<"DENSE_BAND,pair="<<pair<<",begin="<<bandStart[pair]<<",end="<<lastExceeded[pair]<<'\n';
            bandStart[pair]=-1;
        }
    }
    for(int pair=0;pair<2;++pair){
        if(bandStart[pair]>=0)std::cout<<"DENSE_BAND,pair="<<pair<<",begin="<<bandStart[pair]<<",end="<<lastExceeded[pair]<<'\n';
        std::cout<<"DENSE_SUMMARY,pair="<<pair<<",samples="<<distances.size()<<",target_exceedances="<<exceeded[pair]
                 <<",max_error="<<maxError[pair]<<",max_error_distance="<<maxDistance[pair]<<'\n';
    }
    std::cout<<"DENSE_RUNTIME,runtime_s="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<'\n';
}

void circuit(const std::string& name,const Track& track,LapMode mode)
{
    const bool race=isRace(mode);
    std::array<Entry,4> runs;
    std::array<std::vector<TracePoint>,4> traces;
    std::array<double,4> totals{};
    for(std::size_t i=0;i<steps.size();++i){
        const auto start=std::chrono::steady_clock::now();
        auto& e=runs[i];e=presetEntry(2);
        if(race)simulateRace(e,track,1,1.225,steps[i],mode,true,3,10);
        else {e.result=simulateTrack(e.car,track,1,1.225,0,steps[i],mode,true);e.sectors=sectorTimes(e.result);}
        traces[i]=race?e.raceTrace:makeTrace(e.result);
        totals[i]=race?e.raceDuration:e.result.totalTime;
        if(!std::isfinite(totals[i])||totals[i]<=0)throw std::runtime_error("Nonpositive or nonfinite total time");
        for(const auto& p:traces[i])if(!std::isfinite(p.speed)||p.speed<0||!std::isfinite(p.time)||!std::isfinite(p.distance))
            throw std::runtime_error("Invalid convergence trace");
        validSimulation(e.car,e.result,!race);
        validTrace(traces[i],e.result.totalDistance*(race?3:1),totals[i]);
        if(race){
            require(e.raceLaps==3&&e.lapTimes.size()==3,"Invalid race lap count");
            require(std::abs(e.raceStartSpeed-(mode==LapMode::RaceStanding?0:10))<=1e-8,"Broken race start speed");
            double sum=0;for(double lap:e.lapTimes){require(std::isfinite(lap)&&lap>0,"Invalid race lap time");sum+=lap;}
            require(std::abs(sum-e.raceDuration)<=1e-6+1e-10*e.raceDuration,"Broken race lap sum");
            for(int lap=1;lap<3;++lap){
                const double d=lap*e.result.totalDistance;
                const auto a=checkedSample(traces[i],d-1e-5),b=checkedSample(traces[i],d+1e-5);
                require(a.speed>0&&b.speed>0&&std::abs(a.speed-b.speed)<=0.001&&b.time>a.time,"Broken race crossing");
            }
        }
        double maxCell=0;for(const auto& cell:e.result.cells)maxCell=std::max(maxCell,cell.length);
        std::cout<<"GRID,"<<name<<",step="<<steps[i]<<",cells_per_lap="<<e.result.cells.size()<<",max_cell_m="<<maxCell
                 <<",total_time="<<totals[i]<<",runtime_s="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<'\n';
    }
    // Finest grid is only a comparison reference, not ground truth.
    for(std::size_t i=0;i<3;++i)std::cout<<"FINE_REFERENCE_ERROR,"<<name<<",step="<<steps[i]<<",total_time_error="<<std::abs(totals[i]-totals[2])
                                        <<",percent="<<100*std::abs(totals[i]-totals[2])/totals[2]<<",reference=0.125 m (not exact)\n";
    refinement(name+",total_time",totals);
    diagnosticTarget(name+",fine total time",std::abs(totals[1]-totals[2]),0.01+0.001*totals[2]);
    diagnosticTarget(name+",extra total time",std::abs(totals[2]-totals[3]),0.01+0.001*totals[3]);
    for(int sector=0;sector<3;++sector){
        std::array<double,4> values{{runs[0].sectors[sector],runs[1].sectors[sector],runs[2].sectors[sector],runs[3].sectors[sector]}};
        refinement(name+",first_lap_sector="+std::to_string(sector+1),values);
        diagnosticTarget(name+",fine sector="+std::to_string(sector+1),std::abs(values[1]-values[2]),0.01+0.001*values[2]);
        diagnosticTarget(name+",extra sector="+std::to_string(sector+1),std::abs(values[2]-values[3]),0.01+0.001*values[3]);
    }
    if(race)for(int lap=0;lap<3;++lap){
        std::array<double,4> values{{runs[0].lapTimes[lap],runs[1].lapTimes[lap],runs[2].lapTimes[lap],runs[3].lapTimes[lap]}};
        refinement(name+",lap="+std::to_string(lap+1),values);
        diagnosticTarget(name+",fine lap="+std::to_string(lap+1),std::abs(values[1]-values[2]),0.01+0.001*values[2]);
        diagnosticTarget(name+",extra lap="+std::to_string(lap+1),std::abs(values[2]-values[3]),0.01+0.001*values[3]);
    }
    // 201 fixed physical distances span the complete lap/race, including endpoints.
    std::array<double,3> maxError{},maxPercent{};
    std::array<double,2> pairError{};
    double extraMaxError=0,extraMaxDistance=0;
    int extraExceedances=0;
    for(int point=0;point<=200;++point){
        const double distance=traces[2].back().distance*point/200;
        std::array<double,3> v{};for(int i=0;i<3;++i)v[i]=checkedSample(traces[i],distance).speed;
        for(int i=0;i<3;++i){maxError[i]=std::max(maxError[i],std::abs(v[i]-v[2]));if(v[2]>1e-10)maxPercent[i]=std::max(maxPercent[i],100*std::abs(v[i]-v[2])/v[2]);}
        for(int i=0;i<2;++i)pairError[i]=std::max(pairError[i],std::abs(v[i]-v[i+1]));
        const double extraSpeed=checkedSample(traces[3],distance).speed;
        const double extraError=std::abs(v[2]-extraSpeed);
        const double extraLimit=0.02+0.002*std::abs(extraSpeed);
        if(extraError>extraMaxError){extraMaxError=extraError;extraMaxDistance=distance;}
        if(accuracyExceeded(extraError,extraLimit))++extraExceedances;
        std::cout<<"FIXED_SPEED,"<<name<<",distance="<<distance<<",v_0.25="<<v[1]<<",v_0.125="<<v[2]
                 <<",v_0.0625="<<extraSpeed<<",delta_0.25_0.125="<<std::abs(v[1]-v[2])
                 <<",delta_0.125_0.0625="<<extraError<<",extra_target="<<extraLimit
                 <<",extra_status="<<(extraError>extraLimit?"TARGET_EXCEEDED":"WITHIN_TARGET");
        const double originalError=std::abs(v[1]-v[2]);
        if(originalError>1e-10&&extraError>1e-10)std::cout<<",point_refinement_ratio="<<originalError/extraError;
        else std::cout<<",point_refinement_ratio=NA";
        std::cout<<'\n';
        const double limit=0.02+0.002*std::abs(v[2]);
        if(!std::isfinite(v[1])||!std::isfinite(v[2])||std::abs(v[1]-v[2])>limit) {
            diagnosticTarget(name+",speed at distance="+std::to_string(distance),std::abs(v[1]-v[2]),limit);
            // Failure context only; diagnostic values do not change acceptance targets.
            for(int grid=0;grid<3;++grid){
                double begin=0;
                for(std::size_t cell=0;cell<runs[grid].result.cells.size();++cell){
                    const auto& c=runs[grid].result.cells[cell];
                    if(distance>=begin&&distance<=begin+c.length){
                        std::cout<<"DIAGNOSTIC,"<<name<<",step="<<steps[grid]<<",distance="<<distance
                                 <<",sample_speed="<<v[grid]<<",cell_begin="<<begin<<",cell_length="<<c.length
                                 <<",curvature="<<c.curvature<<",grade="<<c.grade<<",density="<<c.density
                                 <<",entry_speed="<<runs[grid].result.speed[cell]<<",exit_speed="<<runs[grid].result.speed[cell+1]<<'\n';
                        break;
                    }
                    begin+=c.length;
                }
            }
        }
    }
    for(int i=0;i<3;++i)std::cout<<"SPEED_ERROR,"<<name<<",step="<<steps[i]<<",max_vs_fine_m_s="<<maxError[i]<<",max_vs_fine_percent="<<maxPercent[i]<<'\n';
    std::cout<<"SPEED_REFINEMENT,"<<name<<",max_coarse_medium="<<pairError[0]<<",max_medium_fine="<<pairError[1];
    if(pairError[0]>1e-10&&pairError[1]>1e-10)std::cout<<",ratio="<<pairError[0]/pairError[1];else std::cout<<",ratio=NA";
    std::cout<<",comparison_points=201; maxima may occur at different distances\n";
    std::cout<<"EXTRA_SPEED_SUMMARY,"<<name<<",max_0.125_0.0625="<<extraMaxError<<",distance="<<extraMaxDistance
             <<",target_exceedances="<<extraExceedances<<",classification=diagnostic";
    if(pairError[1]>1e-10&&extraMaxError>1e-10)std::cout<<",max_refinement_ratio="<<pairError[1]/extraMaxError;
    std::cout<<'\n';
    if(name=="monza.track")denseMonza(traces);
}
}

int main(int argc,char* argv[])
{
    if(argc!=2){std::cerr<<"Usage: validate_convergence <tracks directory>\n";return 2;}
    const auto start=std::chrono::steady_clock::now();std::cout<<std::setprecision(15);
    auto run=[](const std::string& name,const auto& work){try{work();}catch(const std::exception& e){++convergenceValidation::failures;std::cerr<<"MANDATORY_FAIL,"<<name<<",exception="<<e.what()<<'\n';}};
    if(std::string(argv[1])=="--self-test"){
        run("mandatory policy self-test",[]{convergenceValidation::selfTest();});
        return convergenceValidation::failures?1:0;
    }
    run("power straight",[]{convergenceValidation::analyticalStraight();});
    for(int choice : {1,2})run("fictional",[&]{convergenceValidation::circuit("fictional_"+std::to_string(choice),presetTrack(choice),LapMode::Qualifying);});
    for(const char* file : {"monza.track","spa.track","cota.track"})run(file,[&]{convergenceValidation::circuit(file,loadTrackFile(std::filesystem::path(argv[1])/file),LapMode::Qualifying);});
    for(auto mode : {LapMode::RaceStanding,LapMode::RaceRolling})run(modeLabel(mode),[&]{convergenceValidation::circuit(modeLabel(mode),presetTrack(2),mode);});
    std::cout<<"SUMMARY,mandatory_failures="<<convergenceValidation::failures
             <<",diagnostic_target_exceedances="<<convergenceValidation::diagnosticExceedances
             <<",runtime_s="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<'\n';
    return convergenceValidation::failures?1:0;
}
