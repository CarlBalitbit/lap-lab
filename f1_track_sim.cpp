/* Circuit geometry data license (bacinger/f1-circuits):
Copyright (c) 2019-2025 Tomislav Bacinger

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
#include <limits>
#include <stdexcept>
#include <string>
#include <fstream>
#include <array>
#include <locale>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <sstream>
#include <utility>
#include <random>
#include <cstdint>

constexpr char simulatorVersion[] = "0.1.0";

enum class SegmentType
{
    Straight,
    Corner
};

struct ProfilePoint { double distance, x, y, elevation, curvature; };

struct TrackSegment
{
    std::string name;
    SegmentType type;
    double length = 0.0;
    double radius = 0.0;
    double angle = 0.0;
    int direction = 1; // +1 left, -1 right; angle remains a positive magnitude.
};

class Track
{
public:
    std::vector<TrackSegment> segments;
    std::vector<ProfilePoint> profile;
    std::vector<double> turnDistances;
    std::string name = "Custom track";
    std::string dataNote = "User-defined flat track";
    std::array<double, 2> sectorEnds{{0.0, 0.0}}; // Both zero selects equal thirds.

    void addStraight(double length)
    {
        TrackSegment segment{};
        segment.type = SegmentType::Straight;
        segment.length = length;
        segments.push_back(segment);
    }

    void addCorner(double radius, double angle, int direction = 1)
    {
        TrackSegment segment{};
        segment.type = SegmentType::Corner;
        segment.radius = radius;
        segment.angle = angle;
        segment.direction = direction;
        segments.push_back(segment);
    }
};

Track presetTrack(int choice)
{
    if(choice<1 || choice>2) throw std::runtime_error("Unknown fictional preset.");
    Track track;track.name=choice==1?"Balanced circuit":"Low-speed / high-downforce circuit";
    for(int side=0;side<4;++side) {
        track.addStraight(side%2==0 ? (choice==1?500.0:180.0) : (choice==1?300.0:100.0));
        track.addCorner(choice==1?60.0:25.0,90.0,1);
    }
    return track;
}

// Public centerline traces: bacinger/f1-circuits (MIT; license supplied).
// Elevation provenance varies by circuit; see its data note and tracks/CATALOG.md.
// Geometry lives in local .track files. No runtime downloads needed.
Track loadTrackFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    input.imbue(std::locale::classic());
    if(!input) throw std::runtime_error("Could not open track file: "+path.string()+". Keep the tracks folder beside the executable, or supply a track folder as the second argument.");
    auto require=[&](bool valid,const char* reason){
        if(!valid)throw std::runtime_error("Invalid track file "+path.string()+": "+reason);
    };
    auto tag=[&](const char* expected){std::string value;input>>value;require(input.good()&&value==expected,"missing or unexpected field");};
    auto finite=[](double value){return std::isfinite(value);};
    Track track;int version=0;double length=0;std::size_t count=0;
    tag("F1TRACK");input>>version;require(input.good()&&version==1,"unsupported format version");
    tag("NAME");input>>std::quoted(track.name);require(input.good()&&!track.name.empty(),"missing name");
    tag("NOTE");input>>std::quoted(track.dataNote);require(input.good(),"missing data note");
    tag("LENGTH");input>>length;require(input.good()&&finite(length)&&length>0,"invalid lap length");
    tag("SECTORS");input>>track.sectorEnds[0]>>track.sectorEnds[1];
    require(input.good()&&finite(track.sectorEnds[0])&&finite(track.sectorEnds[1])
        &&((track.sectorEnds[0]==0&&track.sectorEnds[1]==0)
        ||(0<track.sectorEnds[0]&&track.sectorEnds[0]<track.sectorEnds[1]&&track.sectorEnds[1]<length)),"invalid sector boundaries");
    tag("TURNS");input>>count;require(input.good()&&count>0&&count<=10000,"invalid turn count");
    track.turnDistances.resize(count);
    double previous=-1;
    for(auto& distance:track.turnDistances){input>>distance;require(input.good()&&finite(distance)&&distance>previous&&distance>=0&&distance<length,"invalid or unordered turn markers");previous=distance;}
    tag("PROFILE");input>>count;require(input.good()&&count>=4&&count<=100000,"invalid profile size");
    track.profile.resize(count);previous=-1;
    for(auto& point:track.profile){
        input>>point.distance>>point.x>>point.y>>point.elevation>>point.curvature;
        require(static_cast<bool>(input)&&finite(point.distance)&&finite(point.x)&&finite(point.y)&&finite(point.elevation)&&finite(point.curvature)
            &&point.distance>previous&&point.distance>=0&&point.distance<=length+1e-6,"invalid or unordered profile samples");
        previous=point.distance;
    }
    input>>std::ws;require(input.eof(),"unexpected trailing data");
    const auto& first=track.profile.front();const auto& last=track.profile.back();
    require(std::abs(first.distance)<1e-6&&std::abs(last.distance-length)<1e-6,"profile does not cover the declared lap length");
    require(std::hypot(last.x-first.x,last.y-first.y)<0.5&&std::abs(last.elevation-first.elevation)<0.1,"circuit profile is not closed");
    previous=0;
    for(std::size_t i=0;i<track.turnDistances.size();++i){
        const double end=i+1<track.turnDistances.size()?(track.turnDistances[i]+track.turnDistances[i+1])/2:length;
        track.addStraight(end-previous);
        track.segments.back().name="T"+std::to_string(i+1)+" / approach and exit";
        previous=end;
    }
    return track;
}

struct CircuitFile
{
    std::filesystem::path path;
    std::string name;
};

std::vector<CircuitFile> discoverCircuits(const std::filesystem::path& directory)
{
    std::vector<CircuitFile> circuits;
    // Read only the header here; full profile validation happens on selection.
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (!file.is_regular_file() || file.path().extension() != ".track") continue;
        std::ifstream input(file.path());
        input.imbue(std::locale::classic());
        std::string magic, field, name;
        int version = 0;
        if (!(input >> magic >> version >> field >> std::quoted(name))
            || magic != "F1TRACK" || version != 1 || field != "NAME" || name.empty()) {
            std::cout << "Skipping invalid circuit header: " << file.path().filename().string() << "\n";
            continue;
        }
        circuits.push_back({file.path(), name});
    }
    if (circuits.empty()) throw std::runtime_error("No valid .track files found in " + directory.string());
    if (circuits.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Too many circuit files for the selection menu.");
    std::sort(circuits.begin(), circuits.end(), [](const CircuitFile& a, const CircuitFile& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.path.filename().string() < b.path.filename().string();
    });
    return circuits;
}

ProfilePoint sampleProfile(const Track& track, double distance)
{
    if(distance<=0)return track.profile.front();
    if(distance>=track.profile.back().distance)return track.profile.back();
    auto it=std::upper_bound(track.profile.begin(),track.profile.end(),distance,
        [](double d,const ProfilePoint& p){return d<p.distance;});
    const auto& b=*it;const auto& a=*(it-1);
    const double f=(distance-a.distance)/(b.distance-a.distance);
    return {distance,a.x+f*(b.x-a.x),a.y+f*(b.y-a.y),a.elevation+f*(b.elevation-a.elevation),a.curvature+f*(b.curvature-a.curvature)};
}

double altitudeDensity(double seaLevelDensity, double altitude)
{
    return seaLevelDensity*std::pow(std::max(0.1,1.0-2.25577e-5*altitude),4.25588);
}

class Car
{
public:
    double mass = 0.0;
    double power = 0.0; // kW delivered at the wheels; no gearbox model.
    double dragCoefficient = 0.0;
    double liftCoefficient = 0.0; // Positive means downforce.
    double frontalArea = 0.0;
    double rearFraction = 0.55; // Static weight and downforce share at rear axle.
    double loadSensitivity = 0.10; // mu decreases as tire load increases.
    double referenceTireLoad = 2000.0; // N; input mu is measured at this load.

    double axleCapacity(double mu, double density, double speed, double share, double grade = 0.0) const
    {
        const double tireLoad = share * (mass * 9.81 / std::sqrt(1.0+grade*grade) + downforce(density, speed)) / 2.0;
        return 2.0 * mu * referenceTireLoad
             * std::pow(tireLoad / referenceTireLoad, 1.0 - loadSensitivity);
    }
    double axleGrip(double mu, double density, double speed,
                    double curvature, double share, double grade = 0.0) const
    {
        const double capacity = axleCapacity(mu, density, speed, share, grade);
        const double lateral = share * mass * speed * speed * std::abs(curvature);
        return std::sqrt(std::max(0.0, (capacity-lateral)*(capacity+lateral)));
    }
    double driveGrip(double mu, double density, double speed, double curvature, double grade = 0.0) const
    {
        return axleGrip(mu, density, speed, curvature, rearFraction, grade);
    }

    double dragForce(double density, double speed) const
    {
        return 0.5 * density * dragCoefficient * frontalArea * speed * speed;
    }

    double downforce(double density, double speed) const
    {
        return 0.5 * density * liftCoefficient * frontalArea * speed * speed;
    }

    double corneringSpeed(double mu, double radius, double density, double grade = 0.0) const
    {
        auto feasible = [&](double speed) {
            const double lateral = mass * speed * speed / radius;
            return lateral * rearFraction <= axleCapacity(mu,density,speed,rearFraction,grade)
                && lateral * (1.0-rearFraction) <= axleCapacity(mu,density,speed,1.0-rearFraction,grade);
        };
        double low = 0.0, high = 10.0;
        while (feasible(high)) {
            high *= 2.0;
            if (!std::isfinite(high) || high > 1e8)
                return std::numeric_limits<double>::infinity(); // Effectively straight within supported speeds.
        }
        for (int i=0;i<80;++i) {
            const double mid = (low+high)*0.5;
            if (feasible(mid)) low=mid; else high=mid;
        }
        return low;
    }

    double cornerLength(double radius, double degrees) const
    {
        return radius * (degrees * 3.14159265358979323846 / 180.0);
    }

    // Friction circle: turning, accelerating and braking share tire grip.
    // Curvature is 0 on a straight and 1/radius in a corner.
    double longitudinalGrip(double mu, double density,
                            double speed, double curvature, double grade = 0.0) const
    {
        return axleGrip(mu,density,speed,curvature,rearFraction,grade)
             + axleGrip(mu,density,speed,curvature,1.0-rearFraction,grade);
    }
};

struct Cell
{
    double length;
    double curvature;
    std::size_t segment;
    double grade = 0.0;
    double elevation = 0.0;
    double density = 0.0;
};

struct SegmentReport
{
    double length = 0.0;
    double time = 0.0;
    double entrySpeed = 0.0;
    double exitSpeed = 0.0;
    double brakingStart = -1.0;
    double brakingSpeed = 0.0;
    double brakingDistance = 0.0;
};

enum class LapMode { SingleRun, StandingStart, Qualifying, RaceStanding, RaceRolling };
bool isRace(LapMode mode) { return mode == LapMode::RaceStanding || mode == LapMode::RaceRolling; }
const char* modeLabel(LapMode mode) {
    if (mode == LapMode::RaceStanding) return "Race / standing start";
    if (mode == LapMode::RaceRolling) return "Race / rolling start";
    return mode == LapMode::Qualifying ? "Qualifying lap" : "Standing start";
}

struct MapPoint { double distance, x, y; };

struct Simulation
{
    std::string trackName, dataNote;
    bool elevationEnabled = true;
    std::vector<std::array<double,3>> elevationTrace;
    std::array<double,4> sectorBounds{{0,0,0,0}};
    std::vector<MapPoint> map;
    std::vector<MapPoint> cornerLabels;
    bool closed = false;
    double closureGap = 0.0;
    std::vector<Cell> cells;
    std::vector<double> speed;
    std::vector<SegmentReport> reports;
    double totalTime = 0.0;
    double totalDistance = 0.0;
};

// Conservative constant-acceleration bounds across one short cell.
// Grip is concave in squared speed for this model, and drag is linear
// in squared speed. Their combined force minima occur at an endpoint.
double brakeBound(const Car& car, double mu, double density,
                  const Cell& cell, double a, double b)
{
    density=cell.density>0?cell.density:density;
    // Drag helps braking; evaluate it together with available tire grip.
    const double first = car.longitudinalGrip(mu, density, a, cell.curvature,cell.grade)
                         + car.dragForce(density, a);
    const double last = car.longitudinalGrip(mu, density, b, cell.curvature,cell.grade)
                        + car.dragForce(density, b);
    return std::min(first, last) / car.mass + 9.81*cell.grade/std::sqrt(1.0+cell.grade*cell.grade);
}

double driveBound(const Car& car, double mu, double density,
                  const Cell& cell, double a, double b)
{
    density=cell.density>0?cell.density:density;
    const double fastest = std::max(a, b);
    const double powerForce = fastest > 0.0
        ? car.power * 1000.0 / fastest
        : std::numeric_limits<double>::infinity();
    const double first = car.driveGrip(mu, density, a, cell.curvature,cell.grade)
                         - car.dragForce(density, a);
    const double last = car.driveGrip(mu, density, b, cell.curvature,cell.grade)
                        - car.dragForce(density, b);
    return std::min(std::min(first, last),
                    powerForce - car.dragForce(density, fastest)) / car.mass - 9.81*cell.grade/std::sqrt(1.0+cell.grade*cell.grade);
}

Simulation simulateTrack(const Car& car, const Track& track,
                         double mu, double density, double startSpeed,
                         double step = 0.25, LapMode mode = LapMode::SingleRun, bool elevationEnabled = true)
{
    auto positive = [](double x) { return std::isfinite(x) && x > 0.0; };
    auto nonnegative = [](double x) { return std::isfinite(x) && x >= 0.0; };
    if (!positive(car.mass) || !positive(car.power) || !positive(car.frontalArea)
        || !nonnegative(car.dragCoefficient) || !nonnegative(car.liftCoefficient)
        || !positive(mu) || !positive(density) || !positive(step)
        || !nonnegative(startSpeed) || track.segments.empty()
        || !std::isfinite(car.rearFraction) || car.rearFraction <= 0 || car.rearFraction >= 1
        || !positive(car.loadSensitivity) || car.loadSensitivity >= 1
        || !positive(car.referenceTireLoad))
        throw std::runtime_error("Invalid car, track, conditions or starting speed.");

    if (mode == LapMode::StandingStart || mode == LapMode::RaceStanding) startSpeed = 0.0;
    Simulation result;
    result.trackName=track.name;result.dataNote=track.dataNote;result.elevationEnabled=elevationEnabled;
    result.reports.resize(track.segments.size());
    std::vector<double> cellLimits;
    double runningDistance=0;
    constexpr std::size_t maxCells = 2000000;
    for (std::size_t s = 0; s < track.segments.size(); ++s)
    {
        const auto& segment = track.segments[s];
        const bool corner = segment.type == SegmentType::Corner;
        if (corner && (!positive(segment.radius) || !positive(segment.angle)
            || (segment.direction != 1 && segment.direction != -1)))
            throw std::runtime_error("Corner radius and angle must be positive.");
        const double length = corner
            ? car.cornerLength(segment.radius, segment.angle) : segment.length;
        if (!positive(length))
            throw std::runtime_error("Segment lengths must be finite and positive.");
        const double count = std::ceil(length / step);
        if (!std::isfinite(count) || count > maxCells - result.cells.size())
            throw std::runtime_error("Track exceeds the calculation size limit.");
        const std::size_t n = static_cast<std::size_t>(count);
        const double limit = corner
            ? car.corneringSpeed(mu, segment.radius, density)
            : std::numeric_limits<double>::infinity();
        if (std::isnan(limit))
            throw std::runtime_error("Numerical overflow in corner speed calculation.");
        result.reports[s].length = length;
        result.totalDistance += length;
        for (std::size_t j = 0; j < n; ++j)
        {
            result.cells.push_back({length / n, corner ? 1.0 / segment.radius : 0.0, s});
            Cell& cell=result.cells.back();
            if(!track.profile.empty()) {
                const auto first=sampleProfile(track,runningDistance);
                const auto last=sampleProfile(track,runningDistance+cell.length);
                const auto middle=sampleProfile(track,runningDistance+cell.length/2);
                cell.curvature=std::max({std::abs(first.curvature),std::abs(last.curvature),std::abs(middle.curvature)});
                cell.elevation=middle.elevation;
                cell.grade=elevationEnabled?(last.elevation-first.elevation)/cell.length:0;
                cell.density=elevationEnabled?altitudeDensity(density,cell.elevation):density;
                cellLimits.push_back(cell.curvature>0 ? car.corneringSpeed(mu,1/cell.curvature,cell.density,cell.grade)
                    : std::numeric_limits<double>::infinity());
            } else { cell.density=density;cellLimits.push_back(limit); }
            runningDistance+=cell.length;
        }
    }

    const double length = result.totalDistance;
    const bool equalSectors = track.sectorEnds[0]==0 && track.sectorEnds[1]==0;
    const double s1 = equalSectors ? length/3 : track.sectorEnds[0];
    const double s2 = equalSectors ? length*2/3 : track.sectorEnds[1];
    if (!std::isfinite(s1) || !std::isfinite(s2) || !(0<s1 && s1<s2 && s2<length))
        throw std::runtime_error("Sector boundaries must satisfy 0 < S1 < S2 < track length.");
    result.sectorBounds = {{0,s1,s2,length}};
    double mapX=0, mapY=0, heading=0, mapDistance=0;
    result.map.push_back({0,0,0});
    for (const auto& cell : result.cells) {
        const auto& segment = track.segments[cell.segment];
        const double curvature = cell.curvature * segment.direction;
        const double middleHeading = heading + curvature*cell.length/2;
        const double chord = curvature==0 ? cell.length
            : 2*std::sin(curvature*cell.length/2)/curvature;
        mapX += chord*std::cos(middleHeading);
        mapY += chord*std::sin(middleHeading);
        heading += curvature*cell.length;
        mapDistance += cell.length;
        result.map.push_back({mapDistance,mapX,mapY});
    }
    double segmentDistance=0;
    for (std::size_t s=0;s<track.segments.size();++s) {
        const auto& segment=track.segments[s];
        if (segment.type==SegmentType::Corner) {
            const double middle=segmentDistance+result.reports[s].length/2;
            auto it=std::lower_bound(result.map.begin(),result.map.end(),middle,
                [](const MapPoint& p,double d){return p.distance<d;});
            result.cornerLabels.push_back(*it);
        }
        segmentDistance+=result.reports[s].length;
    }
    if(!track.profile.empty()) {
        result.map.clear();result.cornerLabels.clear();
        for(const auto& p:track.profile)result.map.push_back({p.distance,p.x,p.y});
        for(double distance:track.turnDistances) {
            const auto p=sampleProfile(track,distance);result.cornerLabels.push_back({distance,p.x,p.y});
        }
        mapX=result.map.back().x-result.map.front().x;
        mapY=result.map.back().y-result.map.front().y;
        const auto& p0=track.profile.front();const auto& p1=track.profile[1];
        const auto& pn=track.profile.back();const auto& pp=track.profile[track.profile.size()-2];
        heading=std::atan2(pn.y-pp.y,pn.x-pp.x)-std::atan2(p1.y-p0.y,p1.x-p0.x);
    }
    double elevationDistance=0;
    for(const auto& cell:result.cells) {
        result.elevationTrace.push_back({{elevationDistance+cell.length/2,cell.elevation,cell.grade}});
        elevationDistance+=cell.length;
    }
    result.closureGap=std::hypot(mapX,mapY);
    // A loaded profile is already checked for endpoint/elevation closure.
    // Its adjacent chords can have different headings when the finish is
    // inside a corner; that sampling difference does not make it an open route.
    result.closed=result.closureGap<0.5 && (!track.profile.empty()
        || std::abs(std::remainder(heading,2*3.14159265358979323846))<0.01);

    // Finite upper bound from engine work, ignoring drag and tire limits:
    // v^3 <= startSpeed^3 + 3 * wheelPower * distance / mass.
    double upper = std::cbrt(startSpeed * startSpeed * startSpeed
        + 3.0 * car.power * 1000.0 * result.totalDistance / car.mass);
    if (mode == LapMode::StandingStart || mode == LapMode::Qualifying)
    {
        // Bound a repeating lap by terminal speed or by the work available
        // since its slowest corner. A zero-drag, unbounded track has no
        // finite fastest repeating lap in this model.
        const double slowest = *std::min_element(cellLimits.begin(), cellLimits.end());
        upper = std::numeric_limits<double>::infinity();
        if (std::isfinite(slowest))
            upper = std::cbrt(slowest * slowest * slowest
                + 3.0 * car.power * 1000.0 * result.totalDistance / car.mass);
        const double dragFactor = 0.5 * density * car.dragCoefficient * car.frontalArea;
        if (dragFactor > 0.0)
            upper = std::min(upper, std::cbrt(car.power * 1000.0 / dragFactor));
        if (!std::isfinite(upper))
            throw std::runtime_error("No finite fastest repeating lap: add drag "
                                     "or a corner with a finite speed limit.");
    }
    if(!track.profile.empty() && elevationEnabled) {
        double minDensity=density,maxDownhill=0,minHeight=track.profile.front().elevation,maxHeight=minHeight;
        for(const auto& cell:result.cells){minDensity=std::min(minDensity,cell.density);maxDownhill=std::max(maxDownhill,-9.81*cell.grade/std::sqrt(1+cell.grade*cell.grade));minHeight=std::min(minHeight,cell.elevation);maxHeight=std::max(maxHeight,cell.elevation);}
        const double dragFactor=0.5*minDensity*car.dragCoefficient*car.frontalArea;
        if(dragFactor>0 && (mode==LapMode::StandingStart || mode==LapMode::Qualifying)) {
            double low=0,high=std::max(100.0,upper);
            auto net=[&](double v){return car.power*1000/v-dragFactor*v*v+car.mass*maxDownhill;};
            while(net(high)>0)high*=2;
            for(int i=0;i<80;++i){const double mid=(low+high)/2;if(net(mid)>0)low=mid;else high=mid;}
            upper=std::max(upper,high);
        } else upper+=std::sqrt(2*9.81*(maxHeight-minHeight));
    }
    if (!positive(upper))
        throw std::runtime_error("Inputs exceed the numerical range of the model.");
    std::vector<double> envelope(result.cells.size() + 1, upper);
    for (std::size_t i = 0; i < result.cells.size(); ++i)
    {
        envelope[i] = std::min(envelope[i], cellLimits[i]);
        envelope[i + 1] = std::min(envelope[i + 1], cellLimits[i]);
    }

    auto backwardPass = [&]()
    {
        for (std::size_t i = result.cells.size(); i-- > 0;)
        {
            const Cell& cell = result.cells[i];
            const double exit = envelope[i + 1];
            if (envelope[i] <= exit) continue;
            double low = exit, high = envelope[i];
            for (int iteration = 0; iteration < 60; ++iteration)
            {
                const double entry = (low + high) * 0.5;
                const double needed = (entry * entry - exit * exit) / (2.0 * cell.length);
                if (needed <= brakeBound(car, mu, density, cell, entry, exit)) low = entry;
                else high = entry;
            }
            envelope[i] = low;
        }
    };
    auto forwardExit = [&](std::size_t i, double entry, double cap)
    {
        const Cell& cell = result.cells[i];
        if ((cap*cap-entry*entry)/(2.0*cell.length) <= driveBound(car,mu,density,cell,entry,cap)) return cap;
        double low = 0.0, high = cap;
        for (int iteration = 0; iteration < 60; ++iteration)
        {
            const double exit = (low + high) * 0.5;
            const double needed = (exit * exit - entry * entry) / (2.0 * cell.length);
            if (needed <= driveBound(car, mu, density, cell, entry, exit)) low = exit;
            else high = exit;
        }
        return low;
    };
    if (mode == LapMode::SingleRun || isRace(mode)) backwardPass();
    else
    {
        bool converged = false;
        std::vector<double> previous(envelope.size());
        for (int pass = 0; pass < 200; ++pass)
        {
            std::copy(envelope.begin(),envelope.end(),previous.begin());
            envelope.front() = envelope.back() = std::min(envelope.front(), envelope.back());
            backwardPass();
            envelope.back() = std::min(envelope.back(), envelope.front());
            for (std::size_t i = 0; i < result.cells.size(); ++i)
                envelope[i + 1] = forwardExit(i, envelope[i], envelope[i + 1]);
            envelope.front() = envelope.back() = std::min(envelope.front(), envelope.back());
            double change = 0.0;
            for (std::size_t i = 0; i < envelope.size(); ++i)
                change = std::max(change, std::abs(previous[i] - envelope[i]));
            if (change < 1e-10) { converged = true; break; }
        }
        if (!converged)
            throw std::runtime_error("Repeating-lap speed profile did not converge.");
        if (mode == LapMode::Qualifying) startSpeed = envelope.front();
    }
    if (startSpeed > envelope.front() + 1e-8)
        throw std::runtime_error("Starting speed is too high to meet the track's "
                                 "grip/braking limits. Lower the starting speed "
                                 "or add more approach distance.");

    // Forward pass: apply engine and shared tire-grip limits, staying below
    // the braking envelope. Speed never jumps at a segment boundary.
    result.speed.resize(envelope.size());
    result.speed[0] = startSpeed;
    for (std::size_t i = 0; i < result.cells.size(); ++i)
    {
        const Cell& cell = result.cells[i];
        const double entry = result.speed[i];
        const double exit = mode == LapMode::Qualifying
            ? envelope[i + 1] : forwardExit(i, entry, envelope[i + 1]);
        const double accel = (exit * exit - entry * entry) / (2.0 * cell.length);
        if (!std::isfinite(accel)
            || accel < -brakeBound(car, mu, density, cell, entry, exit) - 1e-6
            || accel > driveBound(car, mu, density, cell, entry, exit) + 1e-6)
            throw std::runtime_error("Could not resolve a feasible speed profile. "
                                     "Try a smaller distanceStep or less extreme inputs.");
        const double speedSum = entry + exit;
        if (!(speedSum > 0.0))
            throw std::runtime_error("Car cannot move through the track.");
        const double dt = 2.0 * cell.length / speedSum;
        if (!std::isfinite(dt))
            throw std::runtime_error("Non-finite travel time.");
        result.speed[i + 1] = exit;
        result.totalTime += dt;
        auto& report = result.reports[cell.segment];
        if (i == 0 || result.cells[i - 1].segment != cell.segment)
            report.entrySpeed = entry;
        report.exitSpeed = exit;
        report.time += dt;

        // Slowing due solely to drag is not reported as brake application.
        const double meanDrag = 0.5 * (car.dragForce(cell.density, entry)
                                      + car.dragForce(cell.density, exit));
        if (car.mass * (accel+9.81*cell.grade/std::sqrt(1.0+cell.grade*cell.grade)) + meanDrag < -1e-5)
        {
            if (report.brakingStart < 0.0)
            {
                double offset = 0.0;
                for (std::size_t j = i; j > 0 && result.cells[j - 1].segment == cell.segment; --j)
                    offset += result.cells[j - 1].length;
                report.brakingStart = offset;
                report.brakingSpeed = entry;
            }
            report.brakingDistance += cell.length;
        }
    }
    return result;
}

// Exact partial-cell time for the constant-acceleration approximation.
double partialTime(double entry, double accel, double distance)
{
    if (distance <= 0.0) return 0.0;
    const double speed = std::sqrt(std::max(0.0, entry * entry + 2.0 * accel * distance));
    if (entry + speed <= 0.0) throw std::runtime_error("Zero-speed telemetry cell.");
    return 2.0 * distance / (entry + speed);
}

std::array<double, 3> sectorTimes(const Simulation& result)
{
    std::array<double, 3> times{{0.0, 0.0, 0.0}};
    double distance = 0.0;
    for (std::size_t i = 0; i < result.cells.size(); ++i)
    {
        const double length = result.cells[i].length;
        const double entry = result.speed[i], exit = result.speed[i + 1];
        const double accel = (exit * exit - entry * entry) / (2.0 * length);
        for (int sector = 0; sector < 3; ++sector)
        {
            const double first = std::max(distance, result.sectorBounds[sector]);
            const double last = std::min(distance + length, result.sectorBounds[sector + 1]);
            if (last > first)
                times[sector] += partialTime(entry, accel, last - distance)
                              - partialTime(entry, accel, first - distance);
        }
        distance += length;
    }
    return times;
}

int sectorAt(const Simulation& result, double distance)
{
    return distance < result.sectorBounds[1] ? 1 : distance < result.sectorBounds[2] ? 2 : 3;
}

std::string jsonString(const std::string& value);

void writeMapData(std::ostream& out, const Simulation& result)
{
    const double mapRotation=result.trackName=="Monza" ? -65.0 : result.trackName=="Spa-Francorchamps" ? 90.0 : result.trackName=="COTA" ? 180.0 : 0.0;
    out << "const defaultMapRotation=" << mapRotation << ";\n";
    out << "const trackName=" << jsonString(result.trackName) << ",dataNote=" << jsonString(result.dataNote)
        << ",elevationEnabled=" << (result.elevationEnabled?"true":"false") << ",elevationProfile=[";
    for(std::size_t i=0;i<result.elevationTrace.size();i+=16){if(i)out<<',';const auto& p=result.elevationTrace[i];out<<'['<<p[0]<<','<<p[1]<<','<<p[2]<<']';}
    out << "];\nconst bounds=[";
    for (int i=0;i<4;++i) { if(i)out<<','; out<<result.sectorBounds[i]; }
    out << "],trackMap=[";
    // Map sampling is only for drawing; retain exact endpoints and segment joins.
    for (std::size_t i=0;i<result.map.size();++i) {
        if (i && i+1<result.map.size() && result.map.size()==result.cells.size()+1 && i%8 && result.cells[i-1].segment==result.cells[i].segment) continue;
        if(i)out<<',';
        const auto& p=result.map[i]; out<<'['<<p.distance<<','<<p.x<<','<<p.y<<']';
    }
    out << "],turnLabels=[";
    for (std::size_t i=0;i<result.cornerLabels.size();++i) {
        if(i)out<<',';const auto& p=result.cornerLabels[i];out<<'['<<p.distance<<','<<p.x<<','<<p.y<<']';
    }
    out << "];\nconst sectorColors=['#ff1515','#00e5ff','#e5ff00'];\n"
           "const carColors=['#00ff66','#ff8800','#ae00ff','#0099ff','#ff00aa','#ffffff'];\nconst mapClosed=" << (result.closed?"true":"false")
        << ",closureGap=" << result.closureGap << ";\n";
}

const char* reportStyles()
{
    return R"CSS(
:root{color-scheme:dark;--bg:#1c1c1e;--panel:#262628;--line:#414145;--muted:#b5b3ae;--ink:#f2eee5;--teal:#f0c775;--mint:#f0c775;--coral:#f6a96a;--gold:#f2eee5}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px "Segoe UI",system-ui,sans-serif}main{max-width:1440px;margin:auto;padding:0 32px 40px}a{color:var(--mint);text-decoration:none}a:hover{text-decoration:underline}.topbar{display:flex;justify-content:space-between;align-items:center;height:72px;border-bottom:1px solid var(--line);margin-bottom:34px}.brand{font-size:15px;letter-spacing:.05em;font-weight:650;display:flex;align-items:center;gap:12px}.version{font-size:10px;color:var(--muted);letter-spacing:0;font-weight:400}.brand-mark{width:27px;height:27px;border-radius:7px;background:var(--mint);color:var(--bg);display:grid;place-items:center;font-size:18px}.topbar nav{display:flex;gap:24px;align-items:center;font-size:12px;color:var(--muted)}.local-status{display:flex;gap:7px;align-items:center}.local-status:before{content:"";width:6px;height:6px;border-radius:50%;background:var(--mint)}header{display:flex;justify-content:space-between;align-items:center;gap:24px}.eyebrow{font-size:10px;font-weight:650;letter-spacing:.15em;color:var(--muted);text-transform:uppercase}h1{font-size:clamp(26px,3vw,38px);font-weight:600;letter-spacing:-.045em;margin:9px 0 8px}h2{font-size:15px;font-weight:600;margin:0}p{color:var(--muted);line-height:1.6;margin:8px 0}.pill,.badge{border:1px solid var(--line);background:var(--panel);border-radius:7px;padding:9px 13px;font-size:12px;white-space:nowrap}.summary,.stats{display:grid;gap:14px;margin:26px 0 32px}.summary{grid-template-columns:repeat(3,1fr)}.stats{grid-template-columns:repeat(4,1fr)}.summary>div,.stat{padding:22px;background:var(--panel);border:1px solid var(--line);border-radius:10px}.summary small,.stat span,.stat>small{display:block;color:var(--muted);font-size:11px}.summary small:first-child,.stat span{letter-spacing:.08em;text-transform:uppercase}.summary strong,.stat strong{display:block;font-weight:600;font-size:30px;letter-spacing:-.03em;font-variant-numeric:tabular-nums;margin:12px 0 8px}.stat strong small{font-size:14px;color:var(--muted)}.summary>div:first-child,.stat.total{border-top:2px solid var(--mint)}.summary>div:first-child strong,.stat.total strong{color:var(--mint)}.toolbar,.bar{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin:28px 0 14px}.bar p,.hint,.note{font-size:12px}.muted{color:var(--muted)}button,select{border:1px solid var(--line);background:#323235;color:var(--ink);font:inherit;font-size:12px;border-radius:6px;padding:8px 11px}button{cursor:pointer;transition:background .15s,border-color .15s}button:hover{background:#3c3c40;border-color:#686868}button:focus-visible,select:focus-visible,input:focus-visible,a:focus-visible,summary:focus-visible{outline:2px solid var(--mint);outline-offset:3px}button[aria-pressed=true]{border-color:var(--mint);color:var(--mint)}label{font-size:12px;color:var(--muted)}input[type=checkbox]{accent-color:var(--mint);width:14px;height:14px}input[type=range]{width:100%;accent-color:var(--mint);cursor:pointer;height:20px;margin:10px 0}input:disabled{opacity:.35;cursor:not-allowed}.tablewrap{overflow-x:auto;background:var(--panel);border:1px solid var(--line);border-radius:10px;margin:14px 0}table{border-collapse:collapse;width:100%;white-space:nowrap;font-variant-numeric:tabular-nums}th,td{text-align:right;border-bottom:1px solid var(--line);padding:17px 16px}th{background:#2d2d30;text-transform:uppercase;font-size:10px;letter-spacing:.06em;font-weight:550;color:var(--muted)}th:first-child,td:first-child{text-align:left}td small{display:block;font-size:11px;color:var(--muted);margin-top:5px}tbody tr:last-child td{border-bottom:0}tbody tr:hover{background:#ffffff03}.best{color:var(--mint)}.dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:9px}.telemetry-layout{display:grid;grid-template-columns:minmax(340px,.9fr) minmax(0,1.35fr);gap:20px;align-items:start;margin:18px 0}.map-panel{position:sticky;top:20px}.graph-panel{min-width:0}.chart{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:20px 18px 12px;margin:12px 0}.map-panel .chart{margin:0}.charthead{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}.chart p,.chart .legend{font-size:11px}.legend{display:flex;flex-wrap:wrap;gap:10px 18px;color:var(--muted);font-size:12px}.legend label{display:flex;gap:6px;align-items:center;cursor:pointer}.legend i:not(.dot){display:inline-block;width:12px;height:2px;vertical-align:middle;margin-right:5px}.legend .dot{margin:0}.graph-tabs{display:flex;gap:4px;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:5px;margin-bottom:12px;overflow:auto}.graph-tabs button{flex:1;border:0;background:transparent;white-space:nowrap}.graph-tabs button[aria-pressed=true]{background:#423b2c;color:var(--mint)}.graph-view[hidden]{display:none}canvas{display:block;width:100%;height:290px;touch-action:pan-y}.map-panel #trackMap{height:380px!important}.map-panel #mapNote{font-size:11px;margin:10px 0;color:var(--muted)}.map-controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;padding:12px 0;border-bottom:1px solid var(--line)}.map-controls label{display:flex;align-items:center;gap:6px}.playback-control{display:flex;align-items:center;gap:7px}.playback-control input[type=range]{width:100px;margin:0}.playback-control output{min-width:30px;font-variant-numeric:tabular-nums;color:var(--ink)}#play{background:#423b2c;color:var(--mint);border-color:#786747}#playbackStatus{font-size:10px;color:var(--muted)!important}#zoomLevel{font-size:11px;min-width:34px;text-align:center}.chart-controls{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap;margin:12px 0;color:var(--muted);font-size:11px}.chart-controls>div{display:flex;align-items:center;gap:7px}.chart-controls button{padding:5px 10px}#chartZoomLevel{min-width:30px;text-align:center}.lock-control{display:flex;align-items:center;gap:6px}.cursorbar{display:flex;justify-content:space-between;align-items:center;gap:12px;color:var(--muted);font-size:12px;margin-top:18px}.cursorbar strong{font-weight:500;color:var(--ink)}.readout{display:grid;grid-template-columns:repeat(2,1fr);gap:1px;background:var(--line);border:1px solid var(--line);border-radius:8px;overflow:hidden;margin:14px 0}.readout div{background:var(--panel);padding:13px 15px}.readout small{display:block;color:var(--muted);font-size:9px;letter-spacing:.05em;margin-bottom:6px}.readout b{font-size:15px;font-weight:550;font-variant-numeric:tabular-nums}.inspect td,.inspect th{padding:12px 14px;font-size:12px}.links{display:flex;gap:10px;flex-wrap:wrap;margin:28px 0 16px}.links a{border:1px solid var(--line);border-radius:6px;padding:9px 12px;font-size:12px;background:var(--panel)}details{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:15px 18px;margin-top:12px}summary{cursor:pointer;font-size:12px;font-weight:550;color:var(--muted)}details p{font-size:12px}.foot,footer{border-top:1px solid var(--line);padding-top:16px;margin-top:28px;font-size:11px}.foot p,footer p{font-size:11px}.report-note{font-size:12px}.corner-toggle{display:flex;gap:6px;align-items:center;border:1px solid var(--line);border-radius:6px;padding:6px 9px}.sector-key{display:inline-flex;gap:5px;align-items:center;margin-right:12px}.sector-key i{width:12px;height:3px;display:inline-block}.chart fieldset{border:0!important;padding:0!important;margin:14px 0!important}.chart fieldset legend{display:none}.chart fieldset .legend{margin:0}.hint:empty,.note:empty{display:none}
@media(max-width:1000px){main{padding:0 20px 30px}.telemetry-layout{grid-template-columns:1fr}.map-panel{position:static}.map-panel #trackMap{height:300px!important}.summary strong,.stat strong{font-size:26px}.topbar nav{gap:14px}}
@media(max-width:600px){main{padding:0 14px 25px}.topbar{height:60px;margin-bottom:24px}.topbar nav a{display:none}.brand{font-size:13px}header{align-items:start;gap:12px;flex-wrap:wrap}.summary,.stats{grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:20px 0}.summary>div:first-child{grid-column:1/-1}.summary>div,.stat{padding:16px}.summary strong,.stat strong{font-size:25px}h1{font-size:28px}.badge,.pill{font-size:11px}.chart{padding:16px 12px 10px}.map-panel #trackMap{height:260px!important}.graph-panel canvas{height:240px}.map-controls{gap:8px}.tablewrap{max-width:100%}.chart-controls{gap:8px}.local-status{font-size:10px}}
)CSS";
}

void exportTelemetry(const Car& car, const Simulation& result, double mu,
                     double density, LapMode mode,
                     const std::string& htmlPath = "lap_report.html",
                     const std::string& csvPath = "lap_telemetry.csv",
                     std::size_t carIndex = 0)
{
    (void)mu;
    std::ofstream html(htmlPath), csv(csvPath);
    if (!html || !csv) throw std::runtime_error("Could not create the report files.");
    html.imbue(std::locale::classic()); csv.imbue(std::locale::classic());
    html << std::setprecision(15); csv << std::setprecision(15);
    const auto sectors = sectorTimes(result);
    const char* modeName = isRace(mode) ? (mode == LapMode::RaceStanding ? "Race first lap / standing start" : "Race first lap / rolling start") : modeLabel(mode);
    html << R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lap Lab | Lap telemetry</title><style>
)HTML" << reportStyles() << R"HTML(
</style></head><body><main><div class="topbar"><div class="brand"><span class="brand-mark" aria-hidden="true">↗</span>LAP LAB <span class="version">v)HTML" << simulatorVersion << R"HTML(</span></div><nav><a href="#overview">Overview</a><a href="#telemetry">Telemetry</a><span class="local-status">Offline report</span></nav></div><header id="overview"><div><div class="eyebrow">Lap telemetry</div><h1 id="heading"></h1><p id="subtitle"></p></div><div class="pill" id="mode"></div></header>


<section class="stats" id="stats" aria-label="Lap and sector times"></section>
<div class="toolbar" id="telemetry"><div><h2>Telemetry</h2><p class="hint"></p></div><button id="reset">Reset view</button></div>


<div class="telemetry-layout"><aside class="map-panel"><section class="chart"><div class="charthead"><h2>Circuit map</h2><label class="corner-toggle"><input id="showCorners" type="checkbox" checked> Corner labels</label></div><p id="mapNote"></p><div class="map-controls"><button id="play" aria-pressed="false">Play lap</button><label><input id="loop" type="checkbox" checked> Loop</label><label class="playback-control">Speed <input id="playbackSpeed" type="range" min="0.25" max="16" step="0.25" value="4" aria-label="Playback speed"><output id="playbackSpeedValue" for="playbackSpeed">4×</output></label><label>View <select id="orientation"><option value="typical">Circuit diagram</option><option value="north">North up</option><option value="90">Rotate 90°</option><option value="180">Rotate 180°</option><option value="270">Rotate 270°</option></select></label><span id="playbackStatus" style="color:#b5b3ae">Paused</span></div><div class="map-controls"><button id="zoomOut" aria-label="Zoom out">−</button><span id="zoomLevel">100%</span><button id="zoomIn" aria-label="Zoom in">+</button><button id="zoomReset">Fit track</button><label>Follow <select id="followCar"><option value="none">Off</option><option value="0">Follow car</option></select></label></div><canvas id="trackMap" style="height:320px" aria-label="Track map with sector markers and selected position"></canvas></section></aside><div class="graph-panel"><div class="chart-controls"><div><span>Chart zoom</span><button id="chartZoomOut" aria-label="Zoom chart out">−</button><output id="chartZoomLevel">1×</output><button id="chartZoomIn" aria-label="Zoom chart in">+</button><button id="chartZoomReset">Full lap</button></div><label class="lock-control"><input id="lockPosition" type="checkbox">Lock position</label></div><nav class="graph-tabs" aria-label="Telemetry view"><button data-view="speed" aria-pressed="true">Speed</button><button data-view="accel" aria-pressed="false">Acceleration</button><button data-view="force" aria-pressed="false">Forces</button><button data-view="elevation" aria-pressed="false">Elevation</button></nav><section class="readout" aria-live="polite"><div><small>POSITION / SECTOR</small><b id="position"></b></div><div><small>SPEED</small><b id="speed"></b></div><div><small>LONGITUDINAL / LATERAL</small><b id="g"></b></div><div><small>DRIVE / BRAKE FORCE</small><b id="force"></b></div><div><small>ELAPSED / SEGMENT</small><b id="elapsed"></b></div></section><input id="scrub" type="range" min="0" max="10000" value="0" aria-label="Position around lap"><section class="chart graph-view" data-graph="speed"><div class="charthead"><h2>Speed <span style="color:var(--muted)">/ km/h</span></h2><div class="legend"><span><i id="carSpeedSwatch"></i>Vehicle speed</span></div></div><canvas id="speedChart" aria-label="Speed versus distance"></canvas></section>
<section class="chart graph-view" data-graph="accel" hidden><div class="charthead"><h2>Acceleration <span style="color:var(--muted)">/ g</span></h2><div class="legend"><span><i class="car-color-swatch"></i>Longitudinal (+ accelerate, − slow)</span><span><i style="background:var(--gold)"></i>Lateral magnitude</span></div></div><canvas id="accelChart" aria-label="Longitudinal and lateral acceleration versus distance"></canvas></section>
<section class="chart graph-view" data-graph="force" hidden><div class="charthead"><h2>Applied tire force <span style="color:var(--muted)">/ kN</span></h2><div class="legend"><span><i class="car-color-swatch"></i>Driving</span><span><i style="background:var(--coral)"></i>Braking</span></div></div><canvas id="forceChart" aria-label="Driving and braking forces versus distance"></canvas></section>
<section class="chart graph-view" data-graph="elevation" hidden><h2>Elevation / metres above sea level</h2><p id="elevationReadout"></p><canvas id="elevationChart" aria-label="Track elevation profile"></canvas></section></div></div>

<details><summary>Model &amp; circuit notes</summary><p id="circuitNotes"></p><p>Simplified tire grip, wheel power and aerodynamics. No gearbox, tire temperatures, banking or load transfer. Sectors and corner labels may be approximate.</p></details><div class="foot"><a href="lap_telemetry.csv" download>Download telemetry CSV</a> · Offline report · Geometry: <a href="https://github.com/bacinger/f1-circuits">bacinger/f1-circuits (MIT)</a></div>
</main><script>
'use strict';
const lap={mode:")HTML" << modeName << "\",length:" << result.totalDistance
         << ",time:" << result.totalTime << ",mass:" << car.mass << ",carIndex:" << carIndex
         << ",drag:" << 0.5*density*car.dragCoefficient*car.frontalArea
         << ",sectors:[" << sectors[0] << ',' << sectors[1] << ',' << sectors[2] << "]};\nconst cells=[\n";
    csv << "distance_m,time_s,sector,segment,speed_m_s,speed_km_h,longitudinal_acceleration_m_s2,lateral_acceleration_m_s2,drive_force_N,brake_force_N,elevation_m,grade_percent,air_density_kg_m3\n";
    double x = 0.0, time = 0.0;
    for (std::size_t i=0; i<result.cells.size(); ++i)
    {
        const auto& c = result.cells[i];
        const double v = result.speed[i], w = result.speed[i+1];
        const double a = (w*w-v*v)/(2*c.length);
        const double dt = 2*c.length/(v+w);
        const double mid = std::sqrt(std::max(0.0, (v*v+w*w)*0.5));
        const double force = car.mass*(a+9.81*c.grade/std::sqrt(1.0+c.grade*c.grade)) + car.dragForce(c.density,mid);
        const double distance = x+c.length*0.5;
        const int sector = sectorAt(result,distance);
        csv << distance << ',' << time+partialTime(v,a,c.length*0.5) << ',' << sector << ','
            << c.segment+1 << ',' << mid << ',' << mid*3.6 << ',' << a << ','
            << mid*mid*c.curvature << ',' << std::max(0.0,force) << ',' << std::max(0.0,-force) << ',' << c.elevation << ',' << c.grade*100 << ',' << c.density << '\n';
        if (i) html << ",\n";
        html << '[' << x << ',' << x+c.length << ',' << time << ',' << time+dt << ','
             << v << ',' << w << ',' << a << ',' << c.curvature << ',' << c.segment+1 << ',' << c.grade << ',' << 0.5*c.density*car.dragCoefficient*car.frontalArea << ']';
        x += c.length; time += dt;
    }
    html << "\n];\n";
    writeMapData(html,result);
    html << R"HTML(
const $=id=>document.getElementById(id), fmt=(v,n=2)=>v.toFixed(n);
document.querySelectorAll('.car-color-swatch').forEach(el=>el.style.background=carColors[lap.carIndex]);$('carSpeedSwatch').style.background=carColors[lap.carIndex];$('heading').textContent=trackName;$('mode').textContent=lap.mode;
$('subtitle').textContent=`${fmt(lap.length,0)} m · Three sectors`;
$('stats').innerHTML=[['Lap time',lap.time,`${lap.mode} · M:SS.mss`],...lap.sectors.map((t,i)=>[`Sector ${i+1}`,t,`${fmt(bounds[i],0)}–${fmt(bounds[i+1],0)} m`])].map((s,i)=>`<div class="stat ${i===0?'total':''}" style="${i?'border-top:2px solid '+sectorColors[i-1]:''}"><span>${s[0]}</span><strong>${i===0?formatLapTime(s[1]):fmt(s[1],3)} ${i===0?'':'<small>s</small>'}</strong><small>${s[2]}</small></div>`).join('');
function sample(c,x){let d=Math.max(0,Math.min(c[1]-c[0],x-c[0])),v=Math.sqrt(Math.max(0,c[4]**2+2*c[6]*d)),t=c[2]+(d?2*d/(c[4]+v):0),f=lap.mass*(c[6]+9.81*c[9]/Math.sqrt(1+c[9]*c[9]))+c[10]*v*v;return {x,v:v*3.6,a:c[6]/9.81,l:v*v*c[7]/9.81,drive:Math.max(0,f)/1000,brake:Math.max(0,-f)/1000,t,segment:c[8]};}
function at(x){let lo=0,hi=cells.length-1;while(lo<hi){let m=(lo+hi)>>1;if(cells[m][1]<=x)lo=m+1;else hi=m;}return sample(cells[lo],x);}
// Preserve extrema when displaying large data sets. Hover always uses full data.
const points=[];let stride=Math.max(1,Math.ceil(cells.length/2500));
for(let i=0;i<cells.length;i+=stride){const candidates=[];for(let j=i;j<Math.min(cells.length,i+stride);j++)candidates.push(sample(cells[j],cells[j][0]),sample(cells[j],cells[j][1]));const keep=new Set([0,candidates.length-1]);for(const key of ['v','a','l','drive','brake']){let lo=0,hi=0;for(let k=1;k<candidates.length;k++){if(candidates[k][key]<candidates[lo][key])lo=k;if(candidates[k][key]>candidates[hi][key])hi=k;}keep.add(lo);keep.add(hi);}for(const k of [...keep].sort((a,b)=>a-b))points.push(candidates[k]);}
const charts=[{id:'speedChart',keys:['v'],colors:[carColors[lap.carIndex]]},{id:'accelChart',keys:['a','l'],colors:[carColors[lap.carIndex],'#f2eee5']},{id:'forceChart',keys:['drive','brake'],colors:[carColors[lap.carIndex],'#f6a96a']}];
let cursor=0;
for(const ch of charts){let low=0,high=0;for(const p of points)for(const k of ch.keys){low=Math.min(low,p[k]);high=Math.max(high,p[k]);}const pad=Math.max(.1,(high-low)*.12);ch.low=low<0?low-pad:0;ch.high=high+pad;}
function draw(ch){const canvas=$(ch.id);if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight,dpr=window.devicePixelRatio||1;const ctx=prepareCanvas(canvas);const left=49,right=w-15,top=28,bottom=h-30;ch.left=left;ch.right=right;const view=chartWindow();ch.view=view;const px=x=>left+(x-view.start)/view.width*(right-left),py=y=>bottom-(y-ch.low)/(ch.high-ch.low)*(bottom-top);ctx.font='11px system-ui';ctx.lineWidth=1;
for(let i=0;i<3;i++){ctx.fillStyle=i%2?'#ffffff04':'#ffffff08';ctx.fillRect(Math.max(left,px(bounds[i])),top,Math.max(0,Math.min(right,px(bounds[i+1]))-Math.max(left,px(bounds[i]))),bottom-top);ctx.fillStyle='#b5b3ae';ctx.textAlign='center';if(px((bounds[i]+bounds[i+1])/2)>=left&&px((bounds[i]+bounds[i+1])/2)<=right)ctx.fillText(`S${i+1}`,px((bounds[i]+bounds[i+1])/2),16);}
for(let i=0;i<=4;i++){const v=ch.low+(ch.high-ch.low)*i/4,y=py(v);ctx.strokeStyle='#414145';ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();ctx.textAlign='right';ctx.fillStyle='#b5b3ae';ctx.fillText(fmt(v,ch.id==='speedChart'?0:1),left-8,y+4);}
for(let i=0;i<=3;i++){const tick=view.start+view.width*i/3;let x=px(tick);ctx.setLineDash([4,5]);ctx.strokeStyle='#686868';ctx.beginPath();ctx.moveTo(x,top);ctx.lineTo(x,bottom);ctx.stroke();ctx.setLineDash([]);ctx.textAlign=i===0?'left':i===3?'right':'center';ctx.fillStyle='#b5b3ae';ctx.fillText(fmt(tick,0)+' m',x,h-9);}
ctx.save();ctx.beginPath();ctx.rect(left,top,right-left,bottom-top);ctx.clip();ch.keys.forEach((k,n)=>{ctx.beginPath();ctx.strokeStyle=ch.colors[n];ctx.lineWidth=1.7;points.forEach((p,i)=>{const x=px(p.x),y=py(p[k]);if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});ctx.stroke();});
const p=at(cursor);ctx.strokeStyle='#f2eee5';ctx.lineWidth=1;ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(px(cursor),top);ctx.lineTo(px(cursor),bottom);ctx.stroke();ctx.setLineDash([]);ch.keys.forEach((k,n)=>{ctx.fillStyle=ch.colors[n];ctx.beginPath();ctx.arc(px(cursor),py(p[k]),3,0,Math.PI*2);ctx.fill();});ctx.restore();}


function formatLapTime(seconds){const ms=Math.round(seconds*1000);return Math.floor(ms/60000)+':'+String(Math.floor(ms/1000)%60).padStart(2,'0')+'.'+String(ms%1000).padStart(3,'0');}
let playing=false,playbackTime=0,lastFrame=null,animationId=0,mapRotation=defaultMapRotation,renderVersion=0;
const backgroundCache=new WeakMap();
let rotatedGeometry=null,mapZoom=1,mapPanX=0,mapPanY=0,mapDrag=null;
function prepareCanvas(canvas){const ratio=window.devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;if(canvas.width!==Math.round(w*ratio)||canvas.height!==Math.round(h*ratio)){canvas.width=Math.round(w*ratio);canvas.height=Math.round(h*ratio);}const ctx=canvas.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,w,h);return ctx;}
function cachedBackground(canvas,key,paint){const ctx=prepareCanvas(canvas);let cache=backgroundCache.get(canvas);if(!cache||cache.key!==key||cache.width!==canvas.width||cache.height!==canvas.height){const layer=cache&&cache.width===canvas.width&&cache.height===canvas.height?cache.layer:document.createElement('canvas');if(layer.width!==canvas.width||layer.height!==canvas.height){layer.width=canvas.width;layer.height=canvas.height;}const bg=layer.getContext('2d'),ratio=window.devicePixelRatio||1;bg.setTransform(ratio,0,0,ratio,0,0);bg.clearRect(0,0,canvas.clientWidth,canvas.clientHeight);paint(bg);cache={key,width:canvas.width,height:canvas.height,layer};backgroundCache.set(canvas,cache);}ctx.drawImage(cache.layer,0,0,canvas.clientWidth,canvas.clientHeight);return ctx;}
function rotatePoint(p){const a=mapRotation*Math.PI/180;return [p[0],p[1]*Math.cos(a)-p[2]*Math.sin(a),p[1]*Math.sin(a)+p[2]*Math.cos(a)];}
function setPlaying(value){playing=value;$('play').textContent=value?'Pause lap':'Play lap';$('play').setAttribute('aria-pressed',String(value));$('playbackStatus').textContent=value?'Playing':'Paused';if(value){playbackTime=elapsedAt(cursor);if(playbackTime>=lapDuration())playbackTime=0;lastFrame=null;if(!animationId)animationId=requestAnimationFrame(animateLap);}else {cancelAnimationFrame(animationId);animationId=0;lastFrame=null;}}
function animateLap(now){animationId=0;if(!playing)return;if(document.hidden){lastFrame=null;animationId=requestAnimationFrame(animateLap);return;}if(lastFrame===null)lastFrame=now;const dt=(now-lastFrame)/1000;if(dt>=1/30){lastFrame=now;const duration=lapDuration();playbackTime+=dt*Number($('playbackSpeed').value);if(playbackTime>=duration){if($('loop').checked)playbackTime%=duration;else {playbackTime=duration;update(distanceAtTime(playbackTime));setPlaying(false);return;}}update(distanceAtTime(playbackTime));$('playbackStatus').textContent='Playing · '+playbackTime.toFixed(1)+' / '+duration.toFixed(1)+' s';}animationId=requestAnimationFrame(animateLap);}
function redrawMapView(){renderVersion++;$('zoomLevel').textContent=Math.round(mapZoom*100)+'%';update(cursor);}
function zoomMap(value,anchorX=0,anchorY=0){const next=Math.max(1,Math.min(8,value)),ratio=next/mapZoom;mapPanX=anchorX-(anchorX-mapPanX)*ratio;mapPanY=anchorY-(anchorY-mapPanY)*ratio;mapZoom=next;redrawMapView();}
function installMapZoom(){$('showCorners').addEventListener('change',()=>redrawMapView());$('followCar').addEventListener('change',()=>redrawMapView());const canvas=$('trackMap');canvas.style.touchAction='none';$('zoomIn').addEventListener('click',()=>zoomMap(mapZoom*1.3));$('zoomOut').addEventListener('click',()=>zoomMap(mapZoom/1.3));$('zoomReset').addEventListener('click',()=>{mapZoom=1;mapPanX=mapPanY=0;$('followCar').value='none';redrawMapView();});canvas.addEventListener('wheel',e=>{e.preventDefault();const rect=canvas.getBoundingClientRect();zoomMap(mapZoom*Math.exp(-e.deltaY*.0015),e.clientX-rect.left-canvas.clientWidth/2,e.clientY-rect.top-canvas.clientHeight/2);},{passive:false});canvas.addEventListener('pointerdown',e=>{$('followCar').value='none';mapDrag={id:e.pointerId,x:e.clientX,y:e.clientY};canvas.setPointerCapture(e.pointerId);});canvas.addEventListener('pointermove',e=>{if(!mapDrag||e.pointerId!==mapDrag.id)return;mapPanX+=e.clientX-mapDrag.x;mapPanY+=e.clientY-mapDrag.y;mapDrag.x=e.clientX;mapDrag.y=e.clientY;redrawMapView();});const stop=e=>{if(mapDrag&&e.pointerId===mapDrag.id)mapDrag=null;};canvas.addEventListener('pointerup',stop);canvas.addEventListener('pointercancel',stop);canvas.addEventListener('lostpointercapture',stop);}
let chartZoom=1,chartCenter=0,positionLocked=false;
function chartWindow(){const width=bounds[3]/chartZoom,start=Math.max(0,Math.min(bounds[3]-width,chartCenter-width/2));return {start,end:start+width,width};}
function redrawCharts(){charts.forEach(draw);drawElevation(cursor);}
function zoomCharts(value){chartCenter=cursor;chartZoom=Math.max(1,Math.min(32,value));$('chartZoomLevel').textContent=chartZoom.toFixed(chartZoom%1?1:0)+'×';backgroundCache.delete($('elevationChart'));redrawCharts();}
function installChartControls(){
 $('circuitNotes').textContent=dataNote;
 $('playbackSpeed').addEventListener('input',e=>{$('playbackSpeedValue').textContent=Number(e.target.value)+'×';});
 $('chartZoomIn').addEventListener('click',()=>zoomCharts(chartZoom*2));$('chartZoomOut').addEventListener('click',()=>zoomCharts(chartZoom/2));$('chartZoomReset').addEventListener('click',()=>zoomCharts(1));
 $('lockPosition').addEventListener('change',e=>{positionLocked=e.target.checked;$('scrub').disabled=positionLocked;});
 for(const chart of charts)$(chart.id).addEventListener('wheel',e=>{e.preventDefault();zoomCharts(chartZoom*Math.exp(-e.deltaY*.002));},{passive:false});
}
function installPlayback(){installMapZoom();installChartControls();document.querySelectorAll('[data-view]').forEach(button=>button.addEventListener('click',()=>{document.querySelectorAll('[data-view]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));document.querySelectorAll('[data-graph]').forEach(panel=>panel.hidden=panel.dataset.graph!==button.dataset.view);renderVersion++;update(cursor);}));$('play').addEventListener('click',()=>setPlaying(!playing));$('orientation').addEventListener('change',e=>{mapRotation=e.target.value==='typical'?defaultMapRotation:e.target.value==='north'?0:defaultMapRotation+Number(e.target.value);renderVersion++;update(cursor);});$('scrub').addEventListener('pointerdown',()=>setPlaying(false));$('scrub').addEventListener('keydown',()=>setPlaying(false));$('reset').addEventListener('click',()=>{setPlaying(false);playbackTime=0;update(0);});}

function sectorAtDistance(x){return x<bounds[1]?1:x<bounds[2]?2:3;}
function mapAt(d){let lo=0,hi=trackMap.length-1;while(lo<hi){let m=(lo+hi)>>1;if(trackMap[m][0]<d)lo=m+1;else hi=m;}if(!lo)return trackMap[0];const a=trackMap[lo-1],b=trackMap[lo],t=Math.max(0,Math.min(1,(d-a[0])/(b[0]-a[0])));return [d,a[1]+t*(b[1]-a[1]),a[2]+t*(b[2]-a[2])];}
function drawElevation(d){const canvas=$('elevationChart');if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight;let low=Infinity,high=-Infinity;for(const p of elevationProfile){low=Math.min(low,p[1]);high=Math.max(high,p[1]);}const pad=Math.max(2,(high-low)*.15);low-=pad;high+=pad;const view=chartWindow();const px=x=>50+(x-view.start)/view.width*(w-70),py=y=>h-30-(y-low)/(high-low)*(h-55);
const ctx=cachedBackground(canvas,'elevation:'+view.start+':'+view.width,bg=>{bg.font='11px system-ui';bg.fillStyle='#b5b3ae';for(let i=0;i<5;i++){const y=low+(high-low)*i/4;bg.fillText(y.toFixed(0)+' m',2,py(y));bg.strokeStyle='#414145';bg.beginPath();bg.moveTo(50,py(y));bg.lineTo(w-20,py(y));bg.stroke();}bg.save();bg.beginPath();bg.rect(50,20,w-70,h-50);bg.clip();bg.strokeStyle='#d5d5d5';bg.lineWidth=2;bg.beginPath();elevationProfile.forEach((p,i)=>i?bg.lineTo(px(p[0]),py(p[1])):bg.moveTo(px(p[0]),py(p[1])));bg.stroke();bg.restore();});ctx.strokeStyle='#f2eee5';ctx.beginPath();ctx.moveTo(px(d),20);ctx.lineTo(px(d),h-30);ctx.stroke();let lo=0,hi=elevationProfile.length-1;while(lo<hi){const m=(lo+hi)>>1;if(elevationProfile[m][0]<d)lo=m+1;else hi=m;}const p=elevationProfile[lo];$('elevationReadout').textContent=p[1].toFixed(1)+' m altitude · '+(p[2]*100).toFixed(1)+'% gradient · '+(elevationEnabled?'Elevation physics enabled':'Elevation physics disabled');}
function placeCornerLabels(points,path,width,height,reserved,measure){
 const cell=7,route=new Set(),key=(x,y)=>x+','+y;
 // Rasterize the visible track with a little clearance around its stroke.
 for(let i=1;i<path.length;i++){
  const a=path[i-1],b=path[i];if(Math.max(a.x,b.x)<-20||Math.min(a.x,b.x)>width+20||Math.max(a.y,b.y)<-20||Math.min(a.y,b.y)>height+20)continue;
  const steps=Math.max(1,Math.ceil(Math.hypot(b.x-a.x,b.y-a.y)/4));
  for(let j=0;j<=steps;j++){const x=a.x+(b.x-a.x)*j/steps,y=a.y+(b.y-a.y)*j/steps;if(x<-14||x>width+14||y<-14||y>height+14)continue;
   const gx=Math.floor(x/cell),gy=Math.floor(y/cell);for(let dx=-1;dx<=1;dx++)for(let dy=-1;dy<=1;dy++)route.add(key(gx+dx,gy+dy));
  }
 }
 const crossesTrack=(point,end)=>{
  const dx=end.x-point.x,dy=end.y-point.y,length=Math.hypot(dx,dy);
  for(let i=1;i<path.length;i++){
   const a=path[i-1],b=path[i];
   if(Math.max(a.x,b.x)<Math.min(point.x,end.x)||Math.min(a.x,b.x)>Math.max(point.x,end.x)||Math.max(a.y,b.y)<Math.min(point.y,end.y)||Math.min(a.y,b.y)>Math.max(point.y,end.y))continue;
   const ex=b.x-a.x,ey=b.y-a.y,den=dx*ey-dy*ex;
   if(Math.abs(den)<1e-8)continue;
   const ax=a.x-point.x,ay=a.y-point.y,t=(ax*ey-ay*ex)/den,u=(ax*dy-ay*dx)/den;
   if(t>=0&&t<=1&&u>=0&&u<=1&&t*length>4)return true;
  }return false;
 };
 const occupied=reserved.slice(),placed=[];let hidden=0;
 const overlap=(a,b)=>a.x<b.x+b.w+4&&a.x+a.w+4>b.x&&a.y<b.y+b.h+4&&a.y+a.h+4>b.y;
 const onTrack=box=>{for(let x=Math.floor(box.x/cell);x<=Math.floor((box.x+box.w)/cell);x++)for(let y=Math.floor(box.y/cell);y<=Math.floor((box.y+box.h)/cell);y++)if(route.has(key(x,y)))return true;return false;};
 for(const point of points){
  if(point.x<0||point.x>width||point.y<0||point.y>height)continue;
  const w=measure(point.text)+6,h=14,base=Math.atan2(point.y-height/2,point.x-width/2);let best=null;
  for(const distance of [20,28,36,44]){
   for(const angle of [0,.785,-.785,1.57,-1.57,2.355,-2.355,Math.PI]){
    const x=point.x+Math.cos(base+angle)*distance-w/2,y=point.y+Math.sin(base+angle)*distance-h/2,box={x,y,w,h};
    if(x<4||x+w>width-4||y<4||y+h>height-4||occupied.some(other=>overlap(box,other))||onTrack(box))continue;
    const end={x:Math.max(x,Math.min(x+w,point.x)),y:Math.max(y,Math.min(y+h,point.y))};
    if(crossesTrack(point,end))continue;
    best=box;break;
   }if(best)break;
  }
  if(best){occupied.push(best);placed.push({...best,point});}else hidden++;
 }
 return {placed,hidden};
}
let cornerLabelsHidden=0,cornerLabelLayout=null,cornerLabelLayoutKey="";
function drawCornerLabels(ctx,rotated,px,py,width,height){
 cornerLabelsHidden=0;if(!$('showCorners').checked)return;
 ctx.font='9px system-ui';
 const {xmin,xmax,ymin,ymax}=rotatedGeometry;
 const scale=mapZoom*Math.min((width-80)/Math.max(1,xmax-xmin),(height-70)/Math.max(1,ymax-ymin)),margin=70;
 const lx=x=>margin+(x-xmin)*scale,ly=y=>margin+(ymax-y)*scale;
 const key=mapRotation+':'+mapZoom+':'+width+':'+height;
 if(!cornerLabelLayout||cornerLabelLayoutKey!==key){
  const points=turnLabels.map(rotatePoint).map((point,i)=>({x:lx(point[1]),y:ly(point[2]),text:'T'+(i+1)}));
  const path=rotated.map(point=>({x:lx(point[1]),y:ly(point[2])})),reserved=[];
  for(let i=0;i<4;i++){const p=rotatePoint(mapAt(bounds[i]));reserved.push({x:lx(p[1])-7,y:ly(p[2])-8,w:55,h:i===3&&mapClosed?44:30});}
  cornerLabelLayout=placeCornerLabels(points,path,(xmax-xmin)*scale+margin*2,(ymax-ymin)*scale+margin*2,reserved,text=>ctx.measureText(text).width);
  cornerLabelLayoutKey=key;
 }
 const layout=cornerLabelLayout,offsetX=px(xmin)-margin,offsetY=py(ymax)-margin;
 cornerLabelsHidden=layout.hidden;ctx.save();ctx.translate(offsetX,offsetY);ctx.textAlign='center';ctx.textBaseline='middle';
 for(const box of layout.placed){
  if(box.x+box.w+offsetX<0||box.x+offsetX>width||box.y+box.h+offsetY<0||box.y+offsetY>height)continue;
  const x=Math.max(box.x,Math.min(box.x+box.w,box.point.x)),y=Math.max(box.y,Math.min(box.y+box.h,box.point.y));
  ctx.strokeStyle='#8c887f';ctx.lineWidth=.7;ctx.beginPath();ctx.moveTo(box.point.x,box.point.y);ctx.lineTo(x,y);ctx.stroke();
  ctx.fillStyle='#262628';ctx.fillRect(box.x,box.y,box.w,box.h);ctx.strokeStyle='#686868';ctx.strokeRect(box.x,box.y,box.w,box.h);
  ctx.fillStyle='#f2eee5';ctx.fillText(box.point.text,box.x+box.w/2,box.y+box.h/2);
 }
 ctx.restore();ctx.font='11px system-ui';
 ctx.textAlign='left';ctx.textBaseline='alphabetic';
}

function drawMap(d){const canvas=$('trackMap'),w=canvas.clientWidth,h=canvas.clientHeight;if(!rotatedGeometry||rotatedGeometry.rotation!==mapRotation){const points=trackMap.map(rotatePoint);let xmin=Infinity,xmax=-Infinity,ymin=Infinity,ymax=-Infinity;for(const p of points){xmin=Math.min(xmin,p[1]);xmax=Math.max(xmax,p[1]);ymin=Math.min(ymin,p[2]);ymax=Math.max(ymax,p[2]);}rotatedGeometry={rotation:mapRotation,points,xmin,xmax,ymin,ymax};}const {points:rotated,xmin,xmax,ymin,ymax}=rotatedGeometry;const scale=mapZoom*Math.min((w-80)/Math.max(1,xmax-xmin),(h-70)/Math.max(1,ymax-ymin)),px=x=>w/2+mapPanX+(x-(xmin+xmax)/2)*scale,py=y=>h/2+mapPanY-(y-(ymin+ymax)/2)*scale;
if($('followCar').value!=='none'){const target=rotatePoint(mapAt(d));mapPanX=-(target[1]-(xmin+xmax)/2)*scale;mapPanY=(target[2]-(ymin+ymax)/2)*scale;}
const ctx=cachedBackground(canvas,'map'+renderVersion+':'+mapPanX+':'+mapPanY,bg=>{const colors=sectorColors;bg.lineWidth=5;bg.lineCap='round';for(let s=0;s<3;s++){const ps=[rotatePoint(mapAt(bounds[s])),...rotated.filter(p=>p[0]>bounds[s]&&p[0]<bounds[s+1]),rotatePoint(mapAt(bounds[s+1]))];bg.strokeStyle=colors[s];bg.beginPath();ps.forEach((p,i)=>i?bg.lineTo(px(p[1]),py(p[2])):bg.moveTo(px(p[1]),py(p[2])));bg.stroke();}bg.font='11px system-ui';bg.fillStyle='#f2eee5';drawCornerLabels(bg,rotated,px,py,w,h);for(let i=0;i<4;i++){const p=rotatePoint(mapAt(bounds[i]));bg.fillRect(px(p[1])-3,py(p[2])-3,6,6);bg.fillText(i===0?'Start':i===3?'Finish':'S'+i,px(p[1])+8,py(p[2])+14+(i===3&&mapClosed?12:0));}});
const p=rotatePoint(mapAt(d)),ahead=rotatePoint(mapAt(Math.min(bounds[3],d+3))),behind=rotatePoint(mapAt(Math.max(0,d-3)));const heading=Math.atan2(py(ahead[2])-py(behind[2]),px(ahead[1])-px(behind[1]));ctx.save();ctx.translate(px(p[1]),py(p[2]));ctx.rotate(heading);ctx.fillStyle=carColors[lap.carIndex];ctx.strokeStyle='#1c1c1e';ctx.lineWidth=1.5;ctx.beginPath();ctx.moveTo(9,0);ctx.lineTo(-6,-5);ctx.lineTo(-3,0);ctx.lineTo(-6,5);ctx.closePath();ctx.fill();ctx.stroke();ctx.restore();$('mapNote').innerHTML=sectorColors.map((color,i)=>'<span class="sector-key"><i style="background:'+color+'"></i>S'+(i+1)+'</span>').join('')+'<span>'+(mapClosed?'Closed circuit':'Open route')+'</span>'+($('showCorners').checked&&cornerLabelsHidden?' · Zoom for '+cornerLabelsHidden+' more labels':'');drawElevation(d);}

function elapsedAt(d){return at(d).t;}function lapDuration(){return lap.time;}
function distanceAtTime(t){let lo=0,hi=cells.length-1;while(lo<hi){const m=(lo+hi)>>1;if(cells[m][3]<=t)lo=m+1;else hi=m;}const c=cells[lo],dt=Math.max(0,Math.min(c[3]-c[2],t-c[2]));return Math.min(c[1],c[0]+c[4]*dt+.5*c[6]*dt*dt);}
function update(x){cursor=Math.max(0,Math.min(lap.length,x));if(playing&&chartZoom>1)chartCenter=cursor;const p=at(cursor);$('scrub').value=cursor/lap.length*10000;$('position').textContent=`${fmt(cursor,1)} m / S${sectorAtDistance(cursor)}`;$('speed').textContent=fmt(p.v,1)+' km/h';$('g').textContent=`${fmt(p.a)} / ${fmt(p.l)} g`;$('force').textContent=`${fmt(p.drive)} / ${fmt(p.brake)} kN`;$('elapsed').textContent=`${fmt(p.t,3)} s / ${p.segment}`;charts.forEach(draw);drawMap(cursor);}
for(const ch of charts){$(ch.id).addEventListener('pointermove',e=>{if(playing||positionLocked)return;const rect=e.currentTarget.getBoundingClientRect();update(ch.view.start+(e.clientX-rect.left-ch.left)/(ch.right-ch.left)*ch.view.width);});$(ch.id).addEventListener('pointerdown',e=>{if(positionLocked)return;setPlaying(false);const rect=e.currentTarget.getBoundingClientRect();update(ch.view.start+(e.clientX-rect.left-ch.left)/(ch.right-ch.left)*ch.view.width);});}
$('scrub').addEventListener('input',e=>{if(positionLocked)return;setPlaying(false);update(Number(e.target.value)/10000*lap.length);});$('reset').addEventListener('click',()=>update(0));window.addEventListener('resize',()=>update(cursor));installPlayback();update(0);
</script></body></html>)HTML";
    html.flush(); csv.flush();
    if (!html || !csv) throw std::runtime_error("Could not finish writing the report files.");
}

struct TracePoint
{
    double distance, time, speed;
};

struct Entry
{
    bool randomized = false;
    std::uint32_t randomSeed = 0;
    std::string name, slug;
    Car car;
    Simulation result;

    std::array<double, 3> sectors{{0.0, 0.0, 0.0}};
    std::vector<TracePoint> trace;
    double mu = 1.0;
    int raceLaps = 0;
    double raceDuration = 0.0, raceStartSpeed = 0.0;
    std::vector<TracePoint> raceTrace;
    std::vector<double> lapTimes;
};

Entry presetEntry(int choice)
{
    if (choice < 1 || choice > 3) throw std::runtime_error("Unknown preset.");
    Entry e;
    e.car.mass = 800.0; e.car.power = 750.0; e.car.frontalArea = 1.5;
    const char* names[] = {"High Downforce", "Balanced", "Low Downforce"};
    const char* slugs[] = {"high_downforce", "balanced", "low_downforce"};
    const double drag[] = {1.2, 1.0, 0.8}, lift[] = {3.5, 3.0, 2.3};
    e.name = names[choice - 1]; e.slug = slugs[choice - 1];
    e.car.dragCoefficient = drag[choice - 1]; e.car.liftCoefficient = lift[choice - 1];
    return e;
}

// Using raw engine output makes seeded cars reproducible across C++ libraries.
Entry randomEntry(std::uint32_t seed,int number)
{
    std::mt19937 generator(seed);
    auto range=[&](double low,double high){return low+(high-low)*(static_cast<double>(generator())/4294967296.0);};
    Entry entry;entry.randomized=true;entry.randomSeed=seed;
    entry.name="Random car "+std::to_string(number)+" (seed "+std::to_string(seed)+")";
    entry.slug="random_"+std::to_string(seed);
    entry.car.mass=range(780,850);
    entry.car.power=range(650,800);
    entry.car.frontalArea=range(1.4,1.6);
    entry.car.liftCoefficient=range(2.0,4.2);
    entry.car.dragCoefficient=0.65+0.20*(entry.car.liftCoefficient-2.0)+range(-0.04,0.04);
    return entry;
}

std::size_t fastestEntry(const std::vector<Entry>& entries)
{
    return static_cast<std::size_t>(std::min_element(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) { return (a.raceLaps ? a.raceDuration : a.result.totalTime) < (b.raceLaps ? b.raceDuration : b.result.totalTime); }) - entries.begin());
}

std::vector<TracePoint> makeTrace(const Simulation& simulation)
{
    std::vector<TracePoint> trace;
    trace.reserve(simulation.cells.size() + 1);
    double x = 0.0, time = 0.0;
    trace.push_back({0.0, 0.0, simulation.speed.front()});
    for (std::size_t i = 0; i < simulation.cells.size(); ++i)
    {
        x += simulation.cells[i].length;
        time += 2.0 * simulation.cells[i].length / (simulation.speed[i] + simulation.speed[i + 1]);
        trace.push_back({x, time, simulation.speed[i + 1]});
    }
    return trace;
}

// Compare elapsed times at the same distance, never at the same array index
// or the same elapsed time. Interpolation follows each cell's acceleration.
TracePoint sampleTrace(const std::vector<TracePoint>& trace, double distance)
{
    if (distance <= 0.0) return trace.front();
    if (distance >= trace.back().distance) return trace.back();
    auto next = std::upper_bound(trace.begin(), trace.end(), distance,
        [](double x, const TracePoint& p) { return x < p.distance; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    const double acceleration = (b.speed*b.speed - a.speed*a.speed) / (2.0*(b.distance-a.distance));
    const double dx = distance - a.distance;
    const double speed = std::sqrt(std::max(0.0, a.speed*a.speed + 2.0*acceleration*dx));
    return {distance, a.time + partialTime(a.speed, acceleration, dx), speed};
}

void simulateRace(Entry& entry, const Track& track, double mu, double density,
                  double step, LapMode mode, bool elevation, int laps, double startSpeed)
{
    if (!isRace(mode) || laps < 1 || laps > 20) throw std::runtime_error("Race length must be 1 to 20 laps.");
    // The single-lap result supplies map/sector metadata; race speeds replace it below.
    auto first = simulateTrack(entry.car, track, mu, density, 0.0, step, LapMode::Qualifying, elevation);
    if (!first.closed) throw std::runtime_error("Race mode requires a closed circuit.");
    const double length = first.totalDistance;
    const std::size_t cellsPerLap = first.cells.size();
    if (cellsPerLap > 2000000 / static_cast<std::size_t>(laps))
        throw std::runtime_error("Race exceeds calculation limit; choose fewer laps.");
    Track repeated; repeated.name = track.name; repeated.dataNote = track.dataNote;
    for (int lap = 0; lap < laps; ++lap) {
        repeated.segments.insert(repeated.segments.end(), track.segments.begin(), track.segments.end());
        for (std::size_t i = 0; i < track.profile.size(); ++i) {
            if (lap && i == 0) continue;
            auto point = track.profile[i]; point.distance += lap * length;
            repeated.profile.push_back(point);
        }
        for (double turn : track.turnDistances) repeated.turnDistances.push_back(turn + lap * length);
    }
    auto race = simulateTrack(entry.car, repeated, mu, density, startSpeed, step, mode, elevation);
    if (race.cells.size() != cellsPerLap * static_cast<std::size_t>(laps))
        throw std::runtime_error("Race discretization does not match lap boundaries.");
    entry.raceTrace = makeTrace(race);
    entry.raceLaps = laps; entry.raceDuration = race.totalTime;
    entry.raceStartSpeed = race.speed.front(); entry.lapTimes.clear();
    double previous = 0;
    for (int lap = 1; lap <= laps; ++lap) {
        const double crossing = sampleTrace(entry.raceTrace, lap * length).time;
        entry.lapTimes.push_back(crossing - previous); previous = crossing;
    }
    first.speed.assign(race.speed.begin(), race.speed.begin() + cellsPerLap + 1);
    first.totalTime = entry.lapTimes.front();
    first.reports.assign(track.segments.size(), SegmentReport{});
    for (std::size_t i = 0; i < cellsPerLap; ++i) {
        auto& report = first.reports[first.cells[i].segment];
        if (report.length == 0) report.entrySpeed = first.speed[i];
        report.length += first.cells[i].length;
        report.time += 2 * first.cells[i].length / (first.speed[i] + first.speed[i+1]);
        report.exitSpeed = first.speed[i+1];
    }
    entry.result = std::move(first);
    entry.sectors = sectorTimes(entry.result);
}

std::filesystem::path createSessionFolder(const std::filesystem::path& root,
                                         bool comparison, LapMode mode)
{
    namespace fs = std::filesystem;
    fs::create_directories(root);
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm calendar{};
#ifdef _WIN32
    if (localtime_s(&calendar, &seconds) != 0) throw std::runtime_error("Could not read local time.");
#else
    if (!localtime_r(&seconds, &calendar)) throw std::runtime_error("Could not read local time.");
#endif
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream label;
    label << std::put_time(&calendar, "%Y-%m-%d_%H-%M-%S") << '_' << std::setfill('0')
          << std::setw(3) << millis << (comparison ? "_comparison" : "_solo")
          << (mode == LapMode::Qualifying ? "_qualifying" : mode == LapMode::RaceRolling ? "_race_rolling" : mode == LapMode::RaceStanding ? "_race_standing" : "_standing");
    for (int suffix = 0; suffix < 10000; ++suffix)
    {
        const fs::path candidate = root / (label.str() + (suffix ? "_" + std::to_string(suffix) : ""));
        // Atomic creation also protects against two runs starting together.
        if (fs::create_directory(candidate)) return candidate;
    }
    throw std::runtime_error("Could not allocate a unique session folder.");
}

std::string jsonString(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32 || c == '<' || c == '>' || c == '&')
            out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<int>(c) << std::dec;
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}

std::string formatLapTime(double seconds)
{
    if(!std::isfinite(seconds) || seconds<0 || seconds>9e12) return "--:--.---";
    const long long milliseconds=std::llround(seconds*1000.0);
    std::ostringstream out;
    out << milliseconds/60000 << ':' << std::setfill('0') << std::setw(2)
        << (milliseconds/1000)%60 << '.' << std::setw(3) << milliseconds%1000;
    return out.str();
}

void printComparison(const std::vector<Entry>& entries, std::size_t reference)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < entries.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
        { return entries[a].result.totalTime < entries[b].result.totalTime; });
    std::cout << "\n--- SESSION RESULTS ---\nReference: " << entries[reference].name
              << " (car " << reference + 1 << ")\nLap times use M:SS.mss; sectors and differences use seconds.\n";
    std::cout << std::left << std::setw(6) << "Car" << std::setw(20) << "Name"
              << std::right << std::setw(11) << "Lap" << std::setw(11) << "Gap"
              << std::setw(11) << "S1" << std::setw(11) << "S2" << std::setw(11) << "S3" << '\n';
    std::cout << std::fixed << std::setprecision(3);
    for (auto i : order)
    {
        const auto& e = entries[i];
        std::cout << std::left << std::setw(6) << i + 1 << std::setw(20) << e.name
                  << std::right << std::setw(11) << formatLapTime(e.result.totalTime)
                  << std::setw(11) << e.result.totalTime - entries[reference].result.totalTime;
        for (double t : e.sectors) std::cout << std::setw(11) << t;
        std::cout << "\n      Sector differences vs reference: " << std::showpos;
        for (int s = 0; s < 3; ++s) std::cout << "S" << std::noshowpos << s + 1 << ' '
            << std::showpos << e.sectors[s] - entries[reference].sectors[s] << "  ";
        std::cout << std::noshowpos << '\n';
    }
}

void writeComparisonReport(const std::vector<Entry>& entries, std::size_t reference,
                           LapMode mode, double mu, const std::filesystem::path& path);

void saveSession(std::vector<Entry>& entries, const Track& track, double mu, double density,
                 double step, LapMode mode, std::size_t reference,
                 const std::filesystem::path& folder)
{
    namespace fs = std::filesystem;
    const char* modeName = isRace(mode) ? (mode == LapMode::RaceStanding ? "race_standing" : "race_rolling") : mode == LapMode::Qualifying ? "qualifying" : "standing_start";
    std::ofstream summary(folder / "summary.csv"), gaps(folder / "time_gaps.csv"), setup(folder / "session.json");
    if (!summary || !gaps || !setup) throw std::runtime_error("Could not create session summary files.");
    for (auto* out : {&summary, &gaps, &setup}) { out->imbue(std::locale::classic()); *out << std::setprecision(15); }
    summary << "car_id,car_name,lap_mode,lap_time_s,delta_to_reference_s,sector_1_s,sector_2_s,sector_3_s,sector_1_delta_s,sector_2_delta_s,sector_3_delta_s,top_speed_km_h,reference_car_id\n";
    setup << "{\n\"format_version\":4,\"session\":" << jsonString(folder.filename().string())
          << ",\"lap_mode\":" << jsonString(modeName) << ",\"mu\":" << mu
          << ",\"simulator_version\":" << jsonString(simulatorVersion) << ",\"race_laps\":" << entries.front().raceLaps << ",\"start_speed_kmh\":" << entries.front().result.speed.front()*3.6
          << ",\"air_density_kg_m3\":" << density << ",\"distance_step_m\":" << step
          << ",\"reference_car_id\":" << reference + 1 << ",\"sector_policy\":\"configured_distances\",\n\"track\":[";
    for (std::size_t i = 0; i < track.segments.size(); ++i)
    {
        if (i) setup << ',';
        const auto& s = track.segments[i];
        setup << "{\"type\":\"" << (s.type == SegmentType::Straight ? "straight" : "corner")
              << "\",\"length_m\":" << s.length << ",\"radius_m\":" << s.radius << ",\"direction\":" << s.direction << ",\"angle_degrees\":" << s.angle << '}';
    }
    setup << "],\n\"track_name\":" << jsonString(track.name)
          << ",\"track_data_note\":" << jsonString(track.dataNote)
          << ",\"elevation_physics_enabled\":" << (entries.front().result.elevationEnabled?"true":"false")
          << ",\"track_profile\":[";
    for(std::size_t i=0;i<track.profile.size();++i){if(i)setup<<',';const auto& p=track.profile[i];setup<<'['<<p.distance<<','<<p.x<<','<<p.y<<','<<p.elevation<<','<<p.curvature<<']';}
    setup << "],\n\"sector_boundaries_m\":[" << entries.front().result.sectorBounds[1] << ','
          << entries.front().result.sectorBounds[2] << "],\n\"cars\":[";
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        auto& e = entries[i];
        e.trace = makeTrace(e.result);
        const fs::path carFolder = folder / e.slug;
        fs::create_directory(carFolder);
        exportTelemetry(e.car, e.result, mu, density, mode,
                        (carFolder / "lap_report.html").string(), (carFolder / "lap_telemetry.csv").string(), i);
        summary << i + 1 << ',' << e.name << ',' << modeName << ',' << e.result.totalTime << ','
                << e.result.totalTime - entries[reference].result.totalTime;
        for (double t : e.sectors) summary << ',' << t;
        for (int s = 0; s < 3; ++s) summary << ',' << e.sectors[s] - entries[reference].sectors[s];
        summary << ',' << *std::max_element(e.result.speed.begin(), e.result.speed.end()) * 3.6
                << ',' << reference + 1 << '\n';
        if (i) setup << ',';
        setup << "{\"id\":" << i + 1 << ",\"name\":" << jsonString(e.name)
              << ",\"randomized\":" << (e.randomized?"true":"false") << ",\"random_seed\":" << (e.randomized?std::to_string(e.randomSeed):"null") << ",\"folder\":" << jsonString(e.slug) << ",\"mass_kg\":" << e.car.mass
              << ",\"wheel_power_kw\":" << e.car.power << ",\"Cd\":" << e.car.dragCoefficient
              << ",\"Cl\":" << e.car.liftCoefficient << ",\"rear_fraction\":" << e.car.rearFraction << ",\"load_sensitivity\":" << e.car.loadSensitivity << ",\"reference_tire_load_N\":" << e.car.referenceTireLoad << ",\"area_m2\":" << e.car.frontalArea << '}';
    }
    setup << "]\n}\n";
    gaps << "distance_m,sector,car_id,car_name,elapsed_s,speed_km_h,delta_to_reference_s,reference_car_id\n";
    std::vector<double> distances;
    for (const auto& p : entries.front().trace) distances.push_back(p.distance);
    distances.push_back(entries.front().result.sectorBounds[1]); distances.push_back(entries.front().result.sectorBounds[2]);
    std::sort(distances.begin(), distances.end());
    distances.erase(std::unique(distances.begin(), distances.end(),
        [](double a, double b) { return std::abs(a - b) < 1e-9; }), distances.end());
    for (double x : distances)
    {
        const double refTime = sampleTrace(entries[reference].trace, x).time;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto p = sampleTrace(entries[i].trace, x);
            gaps << x << ',' << sectorAt(entries.front().result,x) << ','
                 << i + 1 << ',' << entries[i].name << ',' << p.time << ',' << p.speed * 3.6
                 << ',' << p.time - refTime << ',' << reference + 1 << '\n';
        }
    }
    if (isRace(mode)) {
        std::ofstream results(folder / "race_results.csv"), laps(folder / "race_laps.csv"), telemetry(folder / "race_telemetry.csv");
        for (auto* stream : {&results, &laps, &telemetry}) {
            if (!*stream) throw std::runtime_error("Could not create race exports.");
            stream->imbue(std::locale::classic()); *stream << std::setprecision(15);
        }
        results << "car_id,car_name,laps,race_time_s,gap_to_winner_s,best_lap_s,start_speed_kmh\n";
        laps << "car_id,lap,lap_time_s,crossing_time_s\n";
        telemetry << "car_id,total_distance_m,lap,distance_in_lap_m,time_s,speed_kmh\n";
        for (std::size_t i=0; i<entries.size(); ++i) {
            const auto& e=entries[i]; double crossing=0;
            results << i+1 << ',' << e.name << ',' << e.raceLaps << ',' << e.raceDuration << ','
                    << e.raceDuration-entries[reference].raceDuration << ','
                    << *std::min_element(e.lapTimes.begin(),e.lapTimes.end()) << ',' << e.raceStartSpeed*3.6 << '\n';
            for (std::size_t lap=0;lap<e.lapTimes.size();++lap) {
                crossing+=e.lapTimes[lap]; laps << i+1 << ',' << lap+1 << ',' << e.lapTimes[lap] << ',' << crossing << '\n';
            }
            for (const auto& point:e.raceTrace) {
                const double length=e.result.totalDistance;
                const int completed=std::min(e.raceLaps,static_cast<int>(std::floor((point.distance+1e-7)/length)));
                const double local=completed==e.raceLaps?length:std::max(0.0,point.distance-completed*length);
                telemetry << i+1 << ',' << point.distance << ',' << std::min(completed+1,e.raceLaps) << ',' << local << ',' << point.time << ',' << point.speed*3.6 << '\n';
            }
        }
        for (auto* stream : {&results, &laps, &telemetry}) {stream->flush();if(!*stream)throw std::runtime_error("Could not finish race exports.");}
    }
    writeComparisonReport(entries, reference, mode, mu, folder / "session_report.html");
    for (auto* out : {&summary, &gaps, &setup})
    {
        out->flush();
        if (!*out) throw std::runtime_error("Could not finish writing session summary files.");
    }
    std::ofstream complete(folder / "COMPLETE.txt");
    complete << "Session saved successfully. Open session_report.html.\n";
    complete.flush();
    if (!complete) throw std::runtime_error("Could not write session completion marker.");
}

void writeComparisonReport(const std::vector<Entry>& entries, std::size_t reference,
                           LapMode mode, double mu, const std::filesystem::path& path)
{
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Could not create session report.");
    out.imbue(std::locale::classic()); out << std::setprecision(15);
    out << R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Lap Lab | Session report</title>
<style>
)HTML" << reportStyles() << R"HTML(
</style></head><body><main><div class="topbar"><div class="brand"><span class="brand-mark" aria-hidden="true">↗</span>LAP LAB <span class="version">v)HTML" << simulatorVersion << R"HTML(</span></div><nav><a href="#overview">Overview</a><a href="#telemetry">Telemetry</a><span class="local-status">Offline report</span></nav></div>
<header id="overview"><div><div class="eyebrow">Session overview</div><h1 id="heading"></h1><p id="subtitle"></p></div><div class="badge" id="mode"></div></header>
<div class="summary"><div><small id="winnerLabel">FASTEST LAP</small><strong id="winnerTime"></strong><small id="winnerName"></small></div><div><small>TRACK DISTANCE</small><strong id="length"></strong><small>Circuit length</small></div><div><small>START / FINISH</small><strong id="startMode"></strong><small id="startNote"></small></div></div>
<section><div class="bar"><div><h2 id="resultsTitle">Lap results</h2><p></p></div><label>Reference <select id="reference" aria-label="Reference car"></select></label></div>
<div class="tablewrap"><table><thead><tr id="resultsHeader"><th>Car</th><th>Lap / difference</th><th><span class="sector-key"><i style="background:#ff1515"></i>Sector 1</span> / difference</th><th><span class="sector-key"><i style="background:#00e5ff"></i>Sector 2</span> / difference</th><th><span class="sector-key"><i style="background:#e5ff00"></i>Sector 3</span> / difference</th><th>Top speed</th><th>Telemetry</th></tr></thead><tbody id="results"></tbody></table></div></section>


<section id="telemetry"><div class="bar"><div><h2>Telemetry comparison</h2><p></p></div><button id="reset">Reset position</button></div>
<div class="telemetry-layout"><aside class="map-panel"><section class="chart"><div class="charthead"><h2>Circuit map</h2><label class="corner-toggle"><input id="showCorners" type="checkbox" checked> Corner labels</label></div><p id="mapNote"></p><div class="map-controls"><button id="play" aria-pressed="false">Play lap</button><label><input id="loop" type="checkbox" checked> Loop</label><label class="playback-control">Speed <input id="playbackSpeed" type="range" min="0.25" max="16" step="0.25" value="4" aria-label="Playback speed"><output id="playbackSpeedValue" for="playbackSpeed">4×</output></label><label>View <select id="orientation"><option value="typical">Circuit diagram</option><option value="north">North up</option><option value="90">Rotate 90°</option><option value="180">Rotate 180°</option><option value="270">Rotate 270°</option></select></label><span id="playbackStatus" style="color:#b5b3ae">Paused</span></div><div class="map-controls"><button id="zoomOut" aria-label="Zoom out">−</button><span id="zoomLevel">100%</span><button id="zoomIn" aria-label="Zoom in">+</button><button id="zoomReset">Fit track</button><label>Follow <select id="followCar"><option value="none">Off</option><option value="0">Follow car</option></select></label></div><canvas id="trackMap" style="height:320px" aria-label="Track map with sector markers and selected position"></canvas></section></aside><div class="graph-panel"><div class="chart-controls"><div><span>Chart zoom</span><button id="chartZoomOut" aria-label="Zoom chart out">−</button><output id="chartZoomLevel">1×</output><button id="chartZoomIn" aria-label="Zoom chart in">+</button><button id="chartZoomReset">Full lap</button></div><label class="lock-control"><input id="lockPosition" type="checkbox">Lock position</label></div><nav class="graph-tabs" aria-label="Telemetry view"><button data-view="speed" aria-pressed="true">Speed</button><button data-view="gap" aria-pressed="false">Time gap</button><button data-view="elevation" aria-pressed="false">Elevation</button></nav><div class="chart graph-view" data-graph="speed"><h2>Speed <span class="muted">/ km/h</span></h2><p></p><fieldset style="border:1px solid #414145;border-radius:8px;padding:8px 12px;margin:12px 0"><legend style="color:#b5b3ae;font-size:12px">Cars to compare</legend><div class="legend" id="legend"></div></fieldset><canvas id="speedChart" aria-label="Car speed comparison versus distance"></canvas></div>
<div class="chart graph-view" data-graph="gap" hidden><h2>Time gap <span class="muted">/ seconds</span></h2><p id="gapNote"></p><canvas id="gapChart" aria-label="Elapsed time difference from reference versus distance"></canvas></div>
<div class="cursorbar"><strong id="position"></strong><span id="scrubLabel">Distance along track (m)</span></div><input id="scrub" type="range" min="0" max="10000" value="0" aria-label="Position along track">
<div class="tablewrap"><table class="inspect"><thead><tr id="inspectionHeader"><th>At this position</th><th>Speed</th><th>Elapsed time</th><th>Gap to reference</th></tr></thead><tbody id="inspection"></tbody></table></div><section class="chart graph-view" data-graph="elevation" hidden><h2>Elevation / metres above sea level</h2><p id="elevationReadout"></p><canvas id="elevationChart" aria-label="Track elevation profile"></canvas></section></div></div></section>
<div class="links" id="exports"><a href="summary.csv" download>Lap &amp; sector CSV</a><a href="time_gaps.csv" download>Full time-gap CSV</a><a href="session.json" download>Saved car &amp; track settings</a></div>

<details><summary>Car setups &amp; conditions</summary><p id="conditions"></p><div class="tablewrap"><table><thead><tr><th>Car</th><th>Mass / kg</th><th>Wheel power / kW</th><th>Cd</th><th>Cl</th><th>Area / m²</th><th>Rear fraction</th><th>Load sensitivity</th></tr></thead><tbody id="setups"></tbody></table></div></details>
<details><summary>Model &amp; circuit notes</summary><p id="circuitNotes"></p><p>Positive gaps mean slower than the reference. Exported CSV gaps retain the initial reference. Simplified tire grip, wheel power and aerodynamics; no gearbox, tire temperatures, banking or load transfer.</p></details><footer>Offline report · Geometry: <a href="https://github.com/bacinger/f1-circuits">bacinger/f1-circuits (MIT)</a></footer>
</main><script>
'use strict';
const session={
)HTML";
    out << "name:" << jsonString(path.parent_path().filename().string()) << ",mode:"
        << jsonString(modeLabel(mode))
        << ",mu:" << mu << ",length:" << entries.front().result.totalDistance
        << ",race:" << (isRace(mode)?"true":"false") << ",raceLaps:" << entries.front().raceLaps
        << ",startSpeed:" << entries.front().result.speed.front()*3.6
        << ",reference:" << reference << ",cars:[";
    for (std::size_t i=0; i<entries.size(); ++i)
    {
        if (i) out << ',';
        const auto& e = entries[i];
        out << "{id:" << i+1 << ",name:" << jsonString(e.name) << ",folder:" << jsonString(e.slug)
            << ",lap:" << e.result.totalTime << ",sectors:[" << e.sectors[0] << ',' << e.sectors[1] << ',' << e.sectors[2]
            << "],setup:[" << e.car.mass << ',' << e.car.power << ',' << e.car.dragCoefficient << ','
            << e.car.liftCoefficient << ',' << e.car.frontalArea 
            << "," << e.car.rearFraction << "," << e.car.loadSensitivity << "],trace:[";
        for (std::size_t j=0; j<e.trace.size(); ++j)
        {
            if (j) out << ',';
            const auto& p = e.trace[j];
            out << '[' << p.distance << ',' << p.time << ',' << p.speed << ']';
        }
        out << "],duration:" << e.raceDuration << ",lapTimes:[";
        for (std::size_t j=0;j<e.lapTimes.size();++j){if(j)out<<',';out<<e.lapTimes[j];}
        out << "],raceTrace:[";
        for (std::size_t j=0;j<e.raceTrace.size();++j){if(j)out<<',';const auto& point=e.raceTrace[j];out<<'['<<point.distance<<','<<point.time<<','<<point.speed<<']';}
        out << "]}";
    }
    out << "\n]};\n";
    writeMapData(out,entries.front().result);
    out << R"HTML(
// BEGIN PURE TELEMETRY MATH
function sampleAt(trace,x){
 if(x<=trace[0][0])return {x:trace[0][0],time:trace[0][1],speed:trace[0][2]};
 const last=trace[trace.length-1];if(x>=last[0])return {x:last[0],time:last[1],speed:last[2]};
 let lo=0,hi=trace.length-1;while(lo+1<hi){const m=(lo+hi)>>1;if(trace[m][0]<=x)lo=m;else hi=m;}
 const a=trace[lo],b=trace[hi],dx=x-a[0],acc=(b[2]*b[2]-a[2]*a[2])/(2*(b[0]-a[0]));
 const speed=Math.sqrt(Math.max(0,a[2]*a[2]+2*acc*dx));
 return {x,time:a[1]+(dx?2*dx/(a[2]+speed):0),speed};
}
function atPosition(cars,reference,x){const rt=sampleAt(cars[reference].trace,x).time;return cars.map(c=>{const p=sampleAt(c.trace,x);return {...p,gap:p.time-rt};});}
function thin(points,max=1800){if(points.length<=max)return points;const result=[],stride=Math.ceil(points.length/max);for(let i=0;i<points.length;i+=stride){const end=Math.min(points.length,i+stride);let vmin=i,vmax=i,gmin=i,gmax=i;for(let j=i+1;j<end;j++){if(points[j].v<points[vmin].v)vmin=j;if(points[j].v>points[vmax].v)vmax=j;if(points[j].g<points[gmin].g)gmin=j;if(points[j].g>points[gmax].g)gmax=j;}for(const j of [...new Set([i,end-1,vmin,vmax,gmin,gmax])].sort((a,b)=>a-b))result.push(points[j]);}return result;}
// END PURE TELEMETRY MATH
const $=id=>document.getElementById(id),f=(v,n=3)=>v.toFixed(n),delta=v=>(Math.abs(v)<.0005?'0.000':(v>0?'+':'')+f(v))+' s';
const colors=carColors;
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let reference=session.reference,cursor=0,curves=[],inspectionTime=0;const shown=session.cars.map(()=>true);
const label=c=>`${c.name} #${c.id}`, dot=i=>`<i class="dot" style="background:${colors[i]}"></i>`;
const order=session.cars.map((_,i)=>i).sort((a,b)=>session.cars[a].lap-session.cars[b].lap);
const winner=session.cars[order[0]];
$('heading').textContent=session.cars.length>1?'Where the time goes.':'Your lap, recorded.';
$('subtitle').textContent=`${session.cars.length} car${session.cars.length>1?'s':''} · Three sectors`;
$('heading').textContent=trackName;$('mode').textContent=session.mode;$('winnerLabel').textContent=session.cars.length>1?'FASTEST LAP':'LAP TIME';$('winnerTime').textContent=formatLapTime(winner.lap);$('winnerName').textContent=label(winner);$('length').textContent=f(session.length,1)+' m';
$('startMode').textContent=session.mode==='Qualifying lap'?'Flying lap':'0 km/h';$('startNote').textContent=session.mode==='Qualifying lap'?'Qualifying start':'Standing start';
$('reference').innerHTML=session.cars.map((c,i)=>`<option value="${i}">${esc(label(c))}</option>`).join('');$('reference').value=reference;
$('legend').innerHTML=session.cars.map((c,i)=>`<label><input type="checkbox" data-car="${i}" checked>${dot(i)}${esc(label(c))}</label>`).join('');
$('conditions').textContent=`Tire friction coefficient: ${session.mu}. Same track and mode for every car. Rear-wheel drive; static axle loads; tire load sensitivity enabled. Power is specified at the wheels.`;
$('setups').innerHTML=session.cars.map((c,i)=>`<tr><td>${dot(i)}${esc(label(c))}</td>${c.setup.map(v=>`<td>${f(v,2)}</td>`).join('')}</tr>`).join('');
function refreshReference(){const r=session.cars[reference];$('gapNote').textContent=`Reference: ${label(r)}`;
 $('results').innerHTML=order.map(i=>{const c=session.cars[i],values=[c.lap,...c.sectors],ref=[r.lap,...r.sectors];let top=0;for(const p of c.trace)top=Math.max(top,p[2]*3.6);return `<tr><td>${dot(i)}${esc(label(c))}${i===reference?'<small>REFERENCE</small>':''}</td>${values.map((v,j)=>`<td class="${Math.abs(v-Math.min(...session.cars.map(c=>j===0?c.lap:c.sectors[j-1])))<1e-9?'best':''}">${j===0?formatLapTime(v):f(v)+' s'}<small>${delta(v-ref[j])}</small></td>`).join('')}<td>${f(top,1)} km/h</td><td><a href="${encodeURIComponent(c.folder)}/lap_report.html">Open report</a></td></tr>`;}).join('');
 curves=session.cars.map(c=>thin(c.trace.map(p=>({x:p[0],v:p[2]*3.6,g:p[1]-sampleAt(r.trace,p[0]).time}))));update(distanceAtTime(inspectionTime),inspectionTime);
}
const charts=[{id:'speedChart',key:'v'},{id:'gapChart',key:'g'}];
function draw(chart){const canvas=$(chart.id);if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight,dpr=window.devicePixelRatio||1;const ctx=prepareCanvas(canvas);const left=57,right=w-16,top=29,bottom=h-32;chart.left=left;chart.right=right;let low=0,high=0;curves.forEach((ps,i)=>{if(shown[i])for(const p of ps){low=Math.min(low,p[chart.key]);high=Math.max(high,p[chart.key]);}});const pad=Math.max(chart.key==='v'?1:.02,(high-low)*.12);high+=pad;if(low<0)low-=pad;const view=chartWindow();chart.view=view;const px=x=>left+(x-view.start)/view.width*(right-left),py=y=>bottom-(y-low)/(high-low)*(bottom-top);ctx.font='11px system-ui';ctx.lineWidth=1;
 for(let i=0;i<3;i++){ctx.fillStyle=i%2?'#ffffff03':'#ffffff07';ctx.fillRect(Math.max(left,px(bounds[i])),top,Math.max(0,Math.min(right,px(bounds[i+1]))-Math.max(left,px(bounds[i]))),bottom-top);ctx.textAlign='center';ctx.fillStyle='#b5b3ae';if(px((bounds[i]+bounds[i+1])/2)>=left&&px((bounds[i]+bounds[i+1])/2)<=right)ctx.fillText(`SECTOR ${i+1}`,px((bounds[i]+bounds[i+1])/2),16);}
 for(let i=0;i<=4;i++){const v=low+(high-low)*i/4;ctx.strokeStyle='#414145';ctx.beginPath();ctx.moveTo(left,py(v));ctx.lineTo(right,py(v));ctx.stroke();ctx.textAlign='right';ctx.fillStyle='#b5b3ae';ctx.fillText(f(v,chart.key==='v'?0:2),left-9,py(v)+4);}
 for(let i=0;i<=3;i++){const tick=view.start+view.width*i/3;let x=px(tick);ctx.setLineDash([4,5]);ctx.strokeStyle='#686868';ctx.beginPath();ctx.moveTo(x,top);ctx.lineTo(x,bottom);ctx.stroke();ctx.setLineDash([]);ctx.textAlign=i===0?'left':i===3?'right':'center';ctx.fillText(f(tick,0)+' m',x,h-10);}
 if(chart.key==='g'){ctx.strokeStyle='#cccccc';ctx.beginPath();ctx.moveTo(left,py(0));ctx.lineTo(right,py(0));ctx.stroke();}
 ctx.save();ctx.beginPath();ctx.rect(left,top,right-left,bottom-top);ctx.clip();curves.forEach((ps,i)=>{if(!shown[i])return;ctx.strokeStyle=colors[i];ctx.lineWidth=i===reference?1.5:2;ctx.setLineDash(chart.key==='g'&&i===reference?[5,4]:[]);ctx.beginPath();ps.forEach((p,j)=>{if(j)ctx.lineTo(px(p.x),py(p[chart.key]));else ctx.moveTo(px(p.x),py(p[chart.key]));});ctx.stroke();});ctx.setLineDash([]);
 ctx.strokeStyle='#f2eee5';ctx.lineWidth=1;ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(px(cursor),top);ctx.lineTo(px(cursor),bottom);ctx.stroke();ctx.setLineDash([]);atPosition(session.cars,reference,cursor).forEach((p,i)=>{if(!shown[i])return;ctx.fillStyle=colors[i];ctx.beginPath();ctx.arc(px(cursor),py(chart.key==='v'?p.speed*3.6:p.gap),3,0,Math.PI*2);ctx.fill();});
 ctx.restore();if(!shown.some(Boolean)){ctx.fillStyle='#b5b3ae';ctx.textAlign='center';ctx.fillText('Select a car above to display its trace.',(left+right)/2,(top+bottom)/2);}
}


function formatLapTime(seconds){const ms=Math.round(seconds*1000);return Math.floor(ms/60000)+':'+String(Math.floor(ms/1000)%60).padStart(2,'0')+'.'+String(ms%1000).padStart(3,'0');}
let playing=false,playbackTime=0,lastFrame=null,animationId=0,mapRotation=defaultMapRotation,renderVersion=0;
const backgroundCache=new WeakMap();
let rotatedGeometry=null,mapZoom=1,mapPanX=0,mapPanY=0,mapDrag=null;
function prepareCanvas(canvas){const ratio=window.devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;if(canvas.width!==Math.round(w*ratio)||canvas.height!==Math.round(h*ratio)){canvas.width=Math.round(w*ratio);canvas.height=Math.round(h*ratio);}const ctx=canvas.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,w,h);return ctx;}
function cachedBackground(canvas,key,paint){const ctx=prepareCanvas(canvas);let cache=backgroundCache.get(canvas);if(!cache||cache.key!==key||cache.width!==canvas.width||cache.height!==canvas.height){const layer=cache&&cache.width===canvas.width&&cache.height===canvas.height?cache.layer:document.createElement('canvas');if(layer.width!==canvas.width||layer.height!==canvas.height){layer.width=canvas.width;layer.height=canvas.height;}const bg=layer.getContext('2d'),ratio=window.devicePixelRatio||1;bg.setTransform(ratio,0,0,ratio,0,0);bg.clearRect(0,0,canvas.clientWidth,canvas.clientHeight);paint(bg);cache={key,width:canvas.width,height:canvas.height,layer};backgroundCache.set(canvas,cache);}ctx.drawImage(cache.layer,0,0,canvas.clientWidth,canvas.clientHeight);return ctx;}
function rotatePoint(p){const a=mapRotation*Math.PI/180;return [p[0],p[1]*Math.cos(a)-p[2]*Math.sin(a),p[1]*Math.sin(a)+p[2]*Math.cos(a)];}
function setPlaying(value){playing=value;$('play').textContent=value?'Pause lap':'Play lap';$('play').setAttribute('aria-pressed',String(value));$('playbackStatus').textContent=value?'Playing':'Paused';if(value){playbackTime=inspectionTime;if(playbackTime>=lapDuration())playbackTime=0;lastFrame=null;if(!animationId)animationId=requestAnimationFrame(animateLap);}else {cancelAnimationFrame(animationId);animationId=0;lastFrame=null;}}
function animateLap(now){animationId=0;if(!playing)return;if(document.hidden){lastFrame=null;animationId=requestAnimationFrame(animateLap);return;}if(lastFrame===null)lastFrame=now;const dt=(now-lastFrame)/1000;if(dt>=1/30){lastFrame=now;const duration=lapDuration();playbackTime+=dt*Number($('playbackSpeed').value);if(playbackTime>=duration){if($('loop').checked)playbackTime%=duration;else {playbackTime=duration;update(distanceAtTime(playbackTime),playbackTime);setPlaying(false);return;}}update(distanceAtTime(playbackTime),playbackTime);$('playbackStatus').textContent='Playing · '+playbackTime.toFixed(1)+' / '+duration.toFixed(1)+' s';}animationId=requestAnimationFrame(animateLap);}
function redrawMapView(){renderVersion++;$('zoomLevel').textContent=Math.round(mapZoom*100)+'%';update(cursor,inspectionTime);}
function zoomMap(value,anchorX=0,anchorY=0){const next=Math.max(1,Math.min(8,value)),ratio=next/mapZoom;mapPanX=anchorX-(anchorX-mapPanX)*ratio;mapPanY=anchorY-(anchorY-mapPanY)*ratio;mapZoom=next;redrawMapView();}
function syncFollowOptions(){const select=$('followCar'),previous=select.value;select.innerHTML='<option value="none">Off</option>'+session.cars.map((car,i)=>shown[i]?'<option value="'+i+'">'+esc(label(car))+'</option>':'').join('');select.value=previous!=='none'&&shown[Number(previous)]?previous:'none';if(select.value==='none'&&previous!=='none'){mapPanX=mapPanY=0;}}
function installMapZoom(){$('showCorners').addEventListener('change',()=>redrawMapView());syncFollowOptions();$('followCar').addEventListener('change',()=>redrawMapView());const canvas=$('trackMap');canvas.style.touchAction='none';$('zoomIn').addEventListener('click',()=>zoomMap(mapZoom*1.3));$('zoomOut').addEventListener('click',()=>zoomMap(mapZoom/1.3));$('zoomReset').addEventListener('click',()=>{mapZoom=1;mapPanX=mapPanY=0;$('followCar').value='none';redrawMapView();});canvas.addEventListener('wheel',e=>{e.preventDefault();const rect=canvas.getBoundingClientRect();zoomMap(mapZoom*Math.exp(-e.deltaY*.0015),e.clientX-rect.left-canvas.clientWidth/2,e.clientY-rect.top-canvas.clientHeight/2);},{passive:false});canvas.addEventListener('pointerdown',e=>{$('followCar').value='none';mapDrag={id:e.pointerId,x:e.clientX,y:e.clientY};canvas.setPointerCapture(e.pointerId);});canvas.addEventListener('pointermove',e=>{if(!mapDrag||e.pointerId!==mapDrag.id)return;mapPanX+=e.clientX-mapDrag.x;mapPanY+=e.clientY-mapDrag.y;mapDrag.x=e.clientX;mapDrag.y=e.clientY;redrawMapView();});const stop=e=>{if(mapDrag&&e.pointerId===mapDrag.id)mapDrag=null;};canvas.addEventListener('pointerup',stop);canvas.addEventListener('pointercancel',stop);canvas.addEventListener('lostpointercapture',stop);}
let chartZoom=1,chartCenter=0,positionLocked=false;
function chartWindow(){const width=bounds[3]/chartZoom,start=Math.max(0,Math.min(bounds[3]-width,chartCenter-width/2));return {start,end:start+width,width};}
function redrawCharts(){charts.forEach(draw);drawElevation(cursor);}
function zoomCharts(value){chartCenter=cursor;chartZoom=Math.max(1,Math.min(32,value));$('chartZoomLevel').textContent=chartZoom.toFixed(chartZoom%1?1:0)+'×';backgroundCache.delete($('elevationChart'));redrawCharts();}
function installChartControls(){
 $('circuitNotes').textContent=dataNote;
 $('playbackSpeed').addEventListener('input',e=>{$('playbackSpeedValue').textContent=Number(e.target.value)+'×';});
 $('chartZoomIn').addEventListener('click',()=>zoomCharts(chartZoom*2));$('chartZoomOut').addEventListener('click',()=>zoomCharts(chartZoom/2));$('chartZoomReset').addEventListener('click',()=>zoomCharts(1));
 $('lockPosition').addEventListener('change',e=>{positionLocked=e.target.checked;$('scrub').disabled=positionLocked;});
 for(const chart of charts)$(chart.id).addEventListener('wheel',e=>{e.preventDefault();zoomCharts(chartZoom*Math.exp(-e.deltaY*.002));},{passive:false});
}
function installPlayback(){installMapZoom();installChartControls();document.querySelectorAll('[data-view]').forEach(button=>button.addEventListener('click',()=>{document.querySelectorAll('[data-view]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));document.querySelectorAll('[data-graph]').forEach(panel=>panel.hidden=panel.dataset.graph!==button.dataset.view);renderVersion++;update(cursor,inspectionTime);}));$('play').addEventListener('click',()=>setPlaying(!playing));$('orientation').addEventListener('change',e=>{mapRotation=e.target.value==='typical'?defaultMapRotation:e.target.value==='north'?0:defaultMapRotation+Number(e.target.value);renderVersion++;update(cursor,inspectionTime);});$('scrub').addEventListener('pointerdown',()=>setPlaying(false));$('scrub').addEventListener('keydown',()=>setPlaying(false));$('reset').addEventListener('click',()=>{setPlaying(false);playbackTime=0;update(0);});}

function sectorAtDistance(x){return x<bounds[1]?1:x<bounds[2]?2:3;}
function mapAt(d){let lo=0,hi=trackMap.length-1;while(lo<hi){let m=(lo+hi)>>1;if(trackMap[m][0]<d)lo=m+1;else hi=m;}if(!lo)return trackMap[0];const a=trackMap[lo-1],b=trackMap[lo],t=Math.max(0,Math.min(1,(d-a[0])/(b[0]-a[0])));return [d,a[1]+t*(b[1]-a[1]),a[2]+t*(b[2]-a[2])];}
function drawElevation(d){const canvas=$('elevationChart');if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight;let low=Infinity,high=-Infinity;for(const p of elevationProfile){low=Math.min(low,p[1]);high=Math.max(high,p[1]);}const pad=Math.max(2,(high-low)*.15);low-=pad;high+=pad;const view=chartWindow();const px=x=>50+(x-view.start)/view.width*(w-70),py=y=>h-30-(y-low)/(high-low)*(h-55);
const ctx=cachedBackground(canvas,'elevation:'+view.start+':'+view.width,bg=>{bg.font='11px system-ui';bg.fillStyle='#b5b3ae';for(let i=0;i<5;i++){const y=low+(high-low)*i/4;bg.fillText(y.toFixed(0)+' m',2,py(y));bg.strokeStyle='#414145';bg.beginPath();bg.moveTo(50,py(y));bg.lineTo(w-20,py(y));bg.stroke();}bg.save();bg.beginPath();bg.rect(50,20,w-70,h-50);bg.clip();bg.strokeStyle='#d5d5d5';bg.lineWidth=2;bg.beginPath();elevationProfile.forEach((p,i)=>i?bg.lineTo(px(p[0]),py(p[1])):bg.moveTo(px(p[0]),py(p[1])));bg.stroke();bg.restore();});ctx.strokeStyle='#f2eee5';ctx.beginPath();ctx.moveTo(px(d),20);ctx.lineTo(px(d),h-30);ctx.stroke();let lo=0,hi=elevationProfile.length-1;while(lo<hi){const m=(lo+hi)>>1;if(elevationProfile[m][0]<d)lo=m+1;else hi=m;}const p=elevationProfile[lo];$('elevationReadout').textContent=p[1].toFixed(1)+' m altitude · '+(p[2]*100).toFixed(1)+'% gradient · '+(elevationEnabled?'Elevation physics enabled':'Elevation physics disabled');}
function placeCornerLabels(points,path,width,height,reserved,measure){
 const cell=7,route=new Set(),key=(x,y)=>x+','+y;
 // Rasterize the visible track with a little clearance around its stroke.
 for(let i=1;i<path.length;i++){
  const a=path[i-1],b=path[i];if(Math.max(a.x,b.x)<-20||Math.min(a.x,b.x)>width+20||Math.max(a.y,b.y)<-20||Math.min(a.y,b.y)>height+20)continue;
  const steps=Math.max(1,Math.ceil(Math.hypot(b.x-a.x,b.y-a.y)/4));
  for(let j=0;j<=steps;j++){const x=a.x+(b.x-a.x)*j/steps,y=a.y+(b.y-a.y)*j/steps;if(x<-14||x>width+14||y<-14||y>height+14)continue;
   const gx=Math.floor(x/cell),gy=Math.floor(y/cell);for(let dx=-1;dx<=1;dx++)for(let dy=-1;dy<=1;dy++)route.add(key(gx+dx,gy+dy));
  }
 }
 const crossesTrack=(point,end)=>{
  const dx=end.x-point.x,dy=end.y-point.y,length=Math.hypot(dx,dy);
  for(let i=1;i<path.length;i++){
   const a=path[i-1],b=path[i];
   if(Math.max(a.x,b.x)<Math.min(point.x,end.x)||Math.min(a.x,b.x)>Math.max(point.x,end.x)||Math.max(a.y,b.y)<Math.min(point.y,end.y)||Math.min(a.y,b.y)>Math.max(point.y,end.y))continue;
   const ex=b.x-a.x,ey=b.y-a.y,den=dx*ey-dy*ex;
   if(Math.abs(den)<1e-8)continue;
   const ax=a.x-point.x,ay=a.y-point.y,t=(ax*ey-ay*ex)/den,u=(ax*dy-ay*dx)/den;
   if(t>=0&&t<=1&&u>=0&&u<=1&&t*length>4)return true;
  }return false;
 };
 const occupied=reserved.slice(),placed=[];let hidden=0;
 const overlap=(a,b)=>a.x<b.x+b.w+4&&a.x+a.w+4>b.x&&a.y<b.y+b.h+4&&a.y+a.h+4>b.y;
 const onTrack=box=>{for(let x=Math.floor(box.x/cell);x<=Math.floor((box.x+box.w)/cell);x++)for(let y=Math.floor(box.y/cell);y<=Math.floor((box.y+box.h)/cell);y++)if(route.has(key(x,y)))return true;return false;};
 for(const point of points){
  if(point.x<0||point.x>width||point.y<0||point.y>height)continue;
  const w=measure(point.text)+6,h=14,base=Math.atan2(point.y-height/2,point.x-width/2);let best=null;
  for(const distance of [20,28,36,44]){
   for(const angle of [0,.785,-.785,1.57,-1.57,2.355,-2.355,Math.PI]){
    const x=point.x+Math.cos(base+angle)*distance-w/2,y=point.y+Math.sin(base+angle)*distance-h/2,box={x,y,w,h};
    if(x<4||x+w>width-4||y<4||y+h>height-4||occupied.some(other=>overlap(box,other))||onTrack(box))continue;
    const end={x:Math.max(x,Math.min(x+w,point.x)),y:Math.max(y,Math.min(y+h,point.y))};
    if(crossesTrack(point,end))continue;
    best=box;break;
   }if(best)break;
  }
  if(best){occupied.push(best);placed.push({...best,point});}else hidden++;
 }
 return {placed,hidden};
}
let cornerLabelsHidden=0,cornerLabelLayout=null,cornerLabelLayoutKey="";
function drawCornerLabels(ctx,rotated,px,py,width,height){
 cornerLabelsHidden=0;if(!$('showCorners').checked)return;
 ctx.font='9px system-ui';
 const {xmin,xmax,ymin,ymax}=rotatedGeometry;
 const scale=mapZoom*Math.min((width-80)/Math.max(1,xmax-xmin),(height-70)/Math.max(1,ymax-ymin)),margin=70;
 const lx=x=>margin+(x-xmin)*scale,ly=y=>margin+(ymax-y)*scale;
 const key=mapRotation+':'+mapZoom+':'+width+':'+height;
 if(!cornerLabelLayout||cornerLabelLayoutKey!==key){
  const points=turnLabels.map(rotatePoint).map((point,i)=>({x:lx(point[1]),y:ly(point[2]),text:'T'+(i+1)}));
  const path=rotated.map(point=>({x:lx(point[1]),y:ly(point[2])})),reserved=[];
  for(let i=0;i<4;i++){const p=rotatePoint(mapAt(bounds[i]));reserved.push({x:lx(p[1])-7,y:ly(p[2])-8,w:55,h:i===3&&mapClosed?44:30});}
  cornerLabelLayout=placeCornerLabels(points,path,(xmax-xmin)*scale+margin*2,(ymax-ymin)*scale+margin*2,reserved,text=>ctx.measureText(text).width);
  cornerLabelLayoutKey=key;
 }
 const layout=cornerLabelLayout,offsetX=px(xmin)-margin,offsetY=py(ymax)-margin;
 cornerLabelsHidden=layout.hidden;ctx.save();ctx.translate(offsetX,offsetY);ctx.textAlign='center';ctx.textBaseline='middle';
 for(const box of layout.placed){
  if(box.x+box.w+offsetX<0||box.x+offsetX>width||box.y+box.h+offsetY<0||box.y+offsetY>height)continue;
  const x=Math.max(box.x,Math.min(box.x+box.w,box.point.x)),y=Math.max(box.y,Math.min(box.y+box.h,box.point.y));
  ctx.strokeStyle='#8c887f';ctx.lineWidth=.7;ctx.beginPath();ctx.moveTo(box.point.x,box.point.y);ctx.lineTo(x,y);ctx.stroke();
  ctx.fillStyle='#262628';ctx.fillRect(box.x,box.y,box.w,box.h);ctx.strokeStyle='#686868';ctx.strokeRect(box.x,box.y,box.w,box.h);
  ctx.fillStyle='#f2eee5';ctx.fillText(box.point.text,box.x+box.w/2,box.y+box.h/2);
 }
 ctx.restore();ctx.font='11px system-ui';
 ctx.textAlign='left';ctx.textBaseline='alphabetic';
}

function drawMap(d){const canvas=$('trackMap'),w=canvas.clientWidth,h=canvas.clientHeight;if(!rotatedGeometry||rotatedGeometry.rotation!==mapRotation){const points=trackMap.map(rotatePoint);let xmin=Infinity,xmax=-Infinity,ymin=Infinity,ymax=-Infinity;for(const p of points){xmin=Math.min(xmin,p[1]);xmax=Math.max(xmax,p[1]);ymin=Math.min(ymin,p[2]);ymax=Math.max(ymax,p[2]);}rotatedGeometry={rotation:mapRotation,points,xmin,xmax,ymin,ymax};}const {points:rotated,xmin,xmax,ymin,ymax}=rotatedGeometry;const scale=mapZoom*Math.min((w-80)/Math.max(1,xmax-xmin),(h-70)/Math.max(1,ymax-ymin)),px=x=>w/2+mapPanX+(x-(xmin+xmax)/2)*scale,py=y=>h/2+mapPanY-(y-(ymin+ymax)/2)*scale;
if($('followCar').value!=='none'&&shown[Number($('followCar').value)]){const target=rotatePoint(mapAt(carDistanceAtTime(session.cars[Number($('followCar').value)],inspectionTime)));mapPanX=-(target[1]-(xmin+xmax)/2)*scale;mapPanY=(target[2]-(ymin+ymax)/2)*scale;}
const ctx=cachedBackground(canvas,'map'+renderVersion+':'+mapPanX+':'+mapPanY,bg=>{const colors=sectorColors;bg.lineWidth=5;bg.lineCap='round';for(let s=0;s<3;s++){const ps=[rotatePoint(mapAt(bounds[s])),...rotated.filter(p=>p[0]>bounds[s]&&p[0]<bounds[s+1]),rotatePoint(mapAt(bounds[s+1]))];bg.strokeStyle=colors[s];bg.beginPath();ps.forEach((p,i)=>i?bg.lineTo(px(p[1]),py(p[2])):bg.moveTo(px(p[1]),py(p[2])));bg.stroke();}bg.font='11px system-ui';bg.fillStyle='#f2eee5';drawCornerLabels(bg,rotated,px,py,w,h);for(let i=0;i<4;i++){const p=rotatePoint(mapAt(bounds[i]));bg.fillRect(px(p[1])-3,py(p[2])-3,6,6);bg.fillText(i===0?'Start':i===3?'Finish':'S'+i,px(p[1])+8,py(p[2])+14+(i===3&&mapClosed?12:0));}});
const markers=carMarkersAtTime(inspectionTime);markers.forEach(marker=>{const position=marker.distance,p=rotatePoint(mapAt(position)),ahead=rotatePoint(mapAt(Math.min(bounds[3],position+3))),behind=rotatePoint(mapAt(Math.max(0,position-3)));const heading=Math.atan2(py(ahead[2])-py(behind[2]),px(ahead[1])-px(behind[1]));const collisions=markers.filter(other=>Math.abs(other.distance-position)<12);const lane=collisions.length>1?(collisions.indexOf(marker)-(collisions.length-1)/2)*7:0;const x=px(p[1])-Math.sin(heading)*lane,y=py(p[2])+Math.cos(heading)*lane;ctx.save();ctx.translate(x,y);ctx.rotate(heading);ctx.fillStyle=colors[marker.index];ctx.strokeStyle='#1c1c1e';ctx.lineWidth=1.5;ctx.beginPath();ctx.moveTo(9,0);ctx.lineTo(-6,-5);ctx.lineTo(-3,0);ctx.lineTo(-6,5);ctx.closePath();ctx.fill();ctx.stroke();ctx.restore();ctx.font='bold 11px system-ui';ctx.fillStyle=colors[marker.index];ctx.fillText('#'+(marker.index+1),x+10,y-10-marker.index*10);});$('mapNote').innerHTML=sectorColors.map((color,i)=>'<span class="sector-key"><i style="background:'+color+'"></i>S'+(i+1)+'</span>').join('')+'<span>'+(mapClosed?'Closed circuit':'Open route')+'</span>'+($('showCorners').checked&&cornerLabelsHidden?' · Zoom for '+cornerLabelsHidden+' more labels':'');drawElevation(d);}

function elapsedAt(d){return sampleAt(session.cars[reference].trace,d).time;}function lapDuration(){return Math.max(...session.cars.map(car=>car.lap));}
function carDistanceAtTime(car,t){const trace=car.trace;if(t<=0)return 0;if(t>=car.lap)return session.length;let lo=0,hi=trace.length-1;while(lo+1<hi){const m=(lo+hi)>>1;if(trace[m][1]<=t)lo=m;else hi=m;}const a=trace[lo],b=trace[hi],acc=(b[2]*b[2]-a[2]*a[2])/(2*(b[0]-a[0])),dt=Math.max(0,t-a[1]);return Math.min(b[0],a[0]+a[2]*dt+.5*acc*dt*dt);}
function distanceAtTime(t){return carDistanceAtTime(session.cars[reference],t);}
function carMarkersAtTime(t){return session.cars.map((car,index)=>({index,distance:carDistanceAtTime(car,t)})).filter(marker=>shown[marker.index]);}

function update(x,time=null){cursor=Math.max(0,Math.min(session.length,x));if(playing&&chartZoom>1)chartCenter=cursor;inspectionTime=time===null?elapsedAt(cursor):time;$('scrub').value=cursor/session.length*10000;$('position').textContent=`${f(cursor,1)} m · Sector ${sectorAtDistance(cursor)}`;const samples=atPosition(session.cars,reference,cursor);$('inspection').innerHTML=session.cars.map((c,i)=>`<tr style="opacity:${shown[i]?1:.45}"><td>${dot(i)}${esc(label(c))}</td><td>${f(samples[i].speed*3.6,1)} km/h</td><td>${f(samples[i].time)} s</td><td>${delta(samples[i].gap)}</td></tr>`).join('');charts.forEach(draw);drawMap(cursor);}
$('reference').addEventListener('change',e=>{reference=Number(e.target.value);renderVersion++;refreshReference();if(playing){playbackTime=inspectionTime;lastFrame=null;}});$('legend').addEventListener('change',e=>{const i=Number(e.target.dataset.car);if(Number.isInteger(i)&&i>=0&&i<shown.length){shown[i]=e.target.checked;syncFollowOptions();renderVersion++;update(cursor,inspectionTime);}});$('scrub').addEventListener('input',e=>{if(positionLocked)return;setPlaying(false);if(session.race)update(0,Number(e.target.value)/10000*lapDuration());else update(Number(e.target.value)/10000*session.length);});$('reset').addEventListener('click',()=>update(0));
for(const chart of charts)for(const event of ['pointermove','pointerdown'])$(chart.id).addEventListener(event,e=>{if(positionLocked||event==='pointermove'&&playing)return;if(event==='pointerdown')setPlaying(false);const rect=e.currentTarget.getBoundingClientRect();update(chart.view.start+(e.clientX-rect.left-chart.left)/(chart.right-chart.left)*chart.view.width);});window.addEventListener('resize',()=>update(cursor,inspectionTime));installPlayback();refreshReference();
if(session.race){
 const totalDistance=session.length*session.raceLaps;
 let curveLap=-1,curveReference=-1;
 const originalSetPlaying=setPlaying;
 function distanceOnTrace(trace,t){
  if(t<=0)return 0;if(t>=trace.at(-1)[1])return totalDistance;
  let lo=0,hi=trace.length-1;while(lo+1<hi){const mid=(lo+hi)>>1;if(trace[mid][1]<=t)lo=mid;else hi=mid;}
  const a=trace[lo],b=trace[hi],acc=(b[2]*b[2]-a[2]*a[2])/(2*(b[0]-a[0])),dt=Math.max(0,t-a[1]);
  return Math.max(a[0],Math.min(b[0],a[0]+a[2]*dt+.5*acc*dt*dt));
 }
 function raceState(car,t){
  const total=distanceOnTrace(car.raceTrace,t),finished=t>=car.duration;
  const completed=finished?session.raceLaps:Math.min(session.raceLaps-1,Math.floor((total+1e-7)/session.length));
  return {total,completed,lap:Math.min(completed+1,session.raceLaps),distance:finished?session.length:Math.max(0,total-completed*session.length),finished,speed:finished?0:sampleAt(car.raceTrace,total).speed};
 }
 function selectedLap(){return raceState(session.cars[reference],inspectionTime).lap-1;}
 lapDuration=()=>Math.max(...session.cars.map(car=>car.duration));
 carDistanceAtTime=(car,t)=>raceState(car,t).distance;
 distanceAtTime=t=>carDistanceAtTime(session.cars[reference],t);
 elapsedAt=d=>sampleAt(session.cars[reference].raceTrace,selectedLap()*session.length+d).time;
 atPosition=(cars,ref,x)=>{
  const absolute=selectedLap()*session.length+x,rt=sampleAt(cars[ref].raceTrace,absolute).time;
  return cars.map(car=>{const p=sampleAt(car.raceTrace,absolute);return {...p,gap:p.time-rt};});
 };
 function refreshRaceCurves(){
  const lap=selectedLap();if(curveLap===lap&&curveReference===reference)return;
  curveLap=lap;curveReference=reference;const start=lap*session.length,end=start+session.length,r=session.cars[reference];
  curves=session.cars.map(car=>{
   const points=[sampleAt(car.raceTrace,start),...car.raceTrace.filter(p=>p[0]>start+1e-7&&p[0]<end-1e-7).map(p=>({x:p[0],time:p[1],speed:p[2]})),sampleAt(car.raceTrace,end)];
   return thin(points.map(p=>({x:p.x-start,v:p.speed*3.6,g:p.time-sampleAt(r.raceTrace,p.x).time})));
  });
  $('gapNote').textContent=`Lap ${lap+1} · Reference: ${label(r)}`;
 }
 refreshReference=()=>{
  const winnerIndex=session.cars.reduce((best,car,i)=>car.duration<session.cars[best].duration?i:best,0),winner=session.cars[winnerIndex];
  const order=session.cars.map((_,i)=>i).sort((a,b)=>session.cars[a].duration-session.cars[b].duration);
  $('results').innerHTML=order.map((i,rank)=>{const car=session.cars[i];return `<tr><td>${rank+1}</td><td>${dot(i)}${esc(label(car))}${i===reference?'<small>REFERENCE</small>':''}</td><td>${formatLapTime(car.duration)}<small>${delta(car.duration-winner.duration)}</small></td><td>${session.raceLaps}</td><td>${formatLapTime(Math.min(...car.lapTimes))}</td><td><a href="${encodeURIComponent(car.folder)}/lap_report.html">First lap</a></td></tr>`;}).join('');
  $('winnerTime').textContent=formatLapTime(winner.duration);$('winnerName').textContent=label(winner);
  curveReference=-1;update(distanceAtTime(inspectionTime),inspectionTime);
 };
 update=(x,time=null)=>{
  inspectionTime=Math.max(0,Math.min(lapDuration(),time===null?elapsedAt(Math.max(0,Math.min(session.length,x))):time));
  const states=session.cars.map(car=>raceState(car,inspectionTime)),ref=states[reference];cursor=ref.distance;
  if(playing&&chartZoom>1)chartCenter=cursor;
  $('scrub').value=inspectionTime/lapDuration()*10000;
  $('position').textContent=`${formatLapTime(inspectionTime)} · Lap ${ref.lap}/${session.raceLaps} · ${f(cursor,0)} m`;
  const order=states.map((_,i)=>i).sort((a,b)=>states[b].total-states[a].total||(states[a].finished&&states[b].finished?session.cars[a].duration-session.cars[b].duration:a-b)),leaderIndex=order[0],leader=states[leaderIndex];
  $('inspection').innerHTML=order.map((i,rank)=>{
   const car=session.cars[i],state=states[i],lapsBehind=Math.max(0,Math.floor((leader.total-state.total+1e-7)/session.length));
   const gap=i===leaderIndex?'Leader':lapsBehind?`+${lapsBehind} lap${lapsBehind===1?'':'s'}`:delta(state.finished?car.duration-session.cars[leaderIndex].duration:sampleAt(car.raceTrace,leader.total).time-inspectionTime);
   const lapText=state.finished?'Finished':`${state.lap}/${session.raceLaps}`;
   return `<tr style="opacity:${shown[i]?1:.45}"><td>${rank+1}</td><td>${dot(i)}${esc(label(car))}</td><td>${lapText}</td><td>${f(state.speed*3.6,1)} km/h</td><td>${gap}</td></tr>`;
  }).join('');
  refreshRaceCurves();charts.forEach(draw);drawMap(cursor);
 };
 setPlaying=value=>{originalSetPlaying(value);$('play').textContent=value?'Pause race':'Play race';};
 $('resultsTitle').textContent='Race results';$('winnerLabel').textContent='WINNING RACE TIME';
 $('resultsHeader').innerHTML='<th>Pos</th><th>Car</th><th>Race time / gap</th><th>Laps</th><th>Best lap</th><th>Telemetry</th>';
 $('inspectionHeader').innerHTML='<th>Pos</th><th>Car</th><th>Lap</th><th>Speed</th><th>Gap to leader</th>';
 $('subtitle').textContent=`${session.raceLaps} laps · ${session.cars.length} car${session.cars.length>1?'s':''}`;
 $('startMode').textContent=session.startSpeed===0?'Standing start':f(session.startSpeed,0)+' km/h';
 $('startNote').textContent=session.startSpeed===0?'Launch once; continuous laps':'Shared rolling-start speed';
 $('scrubLabel').textContent='Race clock';$('scrub').setAttribute('aria-label','Race time');
 $('play').textContent='Play race';$('loop').checked=false;
 $('exports').innerHTML='<a href="race_results.csv" download>Race results CSV</a><a href="race_laps.csv" download>Lap times CSV</a><a href="race_telemetry.csv" download>Race telemetry CSV</a><a href="session.json" download>Settings</a>';
 $('conditions').textContent=`Grip: ${session.mu}. Fixed car performance; independent trajectories with no collisions, tire wear, fuel use or pit stops.`;
 inspectionTime=0;refreshReference();
}

</script></body></html>)HTML";
    out.flush();
    if (!out) throw std::runtime_error("Could not finish writing session report.");
}

void printSolo(const Entry& entry, const Track& track, double airDensity)
{
    const Car& car = entry.car;
    const Simulation& result = entry.result;
    const double totalTime = result.totalTime, totalDistance = result.totalDistance;
    const double startingVelocity = result.speed.front(), velocity = result.speed.back();
    const double frictionCoefficient = entry.mu;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n--- SEGMENT RESULTS ---\n";

    for (std::size_t i = 0; i < track.segments.size(); ++i)
    {
        const auto& segment = track.segments[i];
        const auto& report = result.reports[i];
        std::cout << "\n" << (track.profile.empty()?"Segment":segment.name) << " " << i + 1
                  << (segment.type == SegmentType::Straight
                      ? " - Straight\n" : " - Corner\n");
        std::cout << "Length: " << report.length << " m\n";
        std::cout << "Time: " << report.time << " s\n";
        std::cout << "Entry speed: " << report.entrySpeed << " m/s\n";
        std::cout << "Exit speed: " << report.exitSpeed << " m/s\n";
        if (segment.type == SegmentType::Corner)
        {
            std::cout << "Radius: " << segment.radius << " m\n";
            std::cout << "Angle: " << segment.angle << " degrees\n";
            double limit = car.corneringSpeed(frictionCoefficient,
                                              segment.radius, airDensity);
            if (std::isfinite(limit))
                std::cout << "Lateral grip speed limit: " << limit << " m/s\n";
            else
                std::cout << "No finite lateral speed limit in this model.\n";
        }
        if (report.brakingStart >= 0.0)
        {
            std::cout << "First brake application: " << report.brakingStart
                      << " m into this segment\n";
            std::cout << "Speed at first brake application: "
                      << report.brakingSpeed << " m/s\n";
            std::cout << "Distance with brakes applied: "
                      << report.brakingDistance << " m\n";
        }
        else
            std::cout << "No brake application on this segment.\n";
    }

    const auto sectors = sectorTimes(result);
    std::cout << "\n--- SECTOR RESULTS ---\n";
    for (int i = 0; i < 3; ++i)
        std::cout << "Sector " << i + 1 << ": " << sectors[i] << " s ("
                  << result.sectorBounds[i] << " to "
                  << result.sectorBounds[i+1] << " m)\n";

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n========================================\n";
    std::cout << "                 RESULTS\n";
    std::cout << "========================================\n";

    std::cout << "\nMass: " << car.mass << " kg\n";
    std::cout << "Power: " << car.power << " kW\n";
    std::cout << "Power to weight: " << car.power / car.mass << " kW/kg\n";

    std::cout << "\nDrag at starting speed: " << car.dragForce(airDensity, startingVelocity) << " N\n";
    std::cout << "Downforce at starting speed: " << car.downforce(airDensity, startingVelocity) << " N\n";

    std::cout << "\nTotal track length: " << totalDistance << " m\n";
    std::cout << "Total track time: " << formatLapTime(totalTime) << "\n";
    std::cout << "Starting speed: " << startingVelocity << " m/s\n";
    std::cout << "Final speed: " << velocity << " m/s\n";

    std::cout << "\n========================================\n";

}


struct BackInput {};
struct EndInput {};
class SetupInput {
    std::vector<double> answers;
    std::vector<std::uint32_t> seeds;
    std::size_t position=0;
public:
    void replay() { position=0; }
    void undo() {
        if(answers.empty()) { std::cout << "Already at the first question.\n"; return; }
        answers.pop_back(); throw BackInput{};
    }
    std::uint32_t autoSeed(int number) {
        while(seeds.size()<static_cast<std::size_t>(number)) {
            std::random_device source; auto seed=static_cast<std::uint32_t>(source());
            seeds.push_back(seed ? seed : 1);
        }
        return seeds[static_cast<std::size_t>(number-1)];
    }
    double number(const std::string& prompt,double fallback,double low,double high,bool integer=false) {
        if(position<answers.size()) return answers[position++];
        for(;;) {
            std::cout << prompt << " [#: selection; B: back; Q: quit]: ";
            std::string line; if(!std::getline(std::cin,line)) throw EndInput{};
            auto first=line.find_first_not_of(" \t\r");
            auto last=line.find_last_not_of(" \t\r");
            line=first==std::string::npos ? "" : line.substr(first,last-first+1);
            if(line=="b" || line=="B" || line=="back" || line=="Back") { undo(); continue; }
            if(line=="q" || line=="Q" || line=="quit") throw EndInput{};
            double value=fallback;
            if(!line.empty()) {
                std::istringstream parser(line); std::string extra;
                if(!(parser>>value) || (parser>>extra)) { std::cout << "Enter one number per line.\n"; continue; }
            }
            if(!std::isfinite(value) || value<low || value>high || (integer && std::floor(value)!=value)) {
                std::cout << "Enter " << (integer ? "a whole number" : "a number") << " from " << low << " to " << high << ".\n"; continue;
            }
            answers.push_back(value); ++position; return value;
        }
    }
};

int main(int argc, char* argv[])
{
    const auto programDirectory=std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path();
    const std::filesystem::path trackDirectory=argc>2 ? std::filesystem::path(argv[2]) : programDirectory/"tracks";
    const std::filesystem::path outputRoot=argc>1 ? std::filesystem::path(argv[1]) : programDirectory/"LapData";
    std::cout << "\nLAP LAB - F1 LAP TIME SIMULATOR v" << simulatorVersion << "\nEnter accepts the preset default. B goes back one answer; Q quits.\n";
    try {
    for(;;) {
    SetupInput input;
    for(;;) {
    input.replay();
    auto readNumber=[&](const char* prompt,double& value) {
        std::string label(prompt);
        double low=0,high=1e9;
        if(label.find("seed")!=std::string::npos) high=4294967295.0;
        else if(label.find("fraction")!=std::string::npos || label.find("sensitivity")!=std::string::npos) { low=0.000001; high=0.999999; }
        else if(label.find("coefficient (C")==std::string::npos) low=0.000001;
        value=input.number(label.empty() ? "Value" : label,value,low,high,label.find("seed")!=std::string::npos);
        return true;
    };
    auto readInteger=[&](const char* prompt,int& value,int low,int high) {
        value=static_cast<int>(input.number(prompt,value>=low && value<=high ? value : low,low,high,true)); return true;
    };
    try {
    int settingsChoice=1;
    std::cout << "\n--- SETTINGS MODE ---\n1. Basic (quick setup)\n2. Advanced (full controls)\n";
    if(!readInteger("Select settings mode: ",settingsChoice,1,2))return 1;
    const bool advancedSettings=settingsChoice==2;
    if(!advancedSettings)
        std::cout << "Basic defaults: grip mu 1.0; track-default sectors; standard traction; elevation enabled for real circuits. Random cars receive a new seed automatically.\n";
    int sessionChoice = 1;
    std::cout << "\n1. Solo run\n2. Compare multiple cars\n";
    if (!readInteger("Select a session: ", sessionChoice, 1, 2)) return 1;
    std::vector<Entry> entries;
    auto chooseCar = [&](int number)
    {
        int choice = 2;
        std::cout << "\n--- CAR " << number << " ---\n"
                  << "1. High Downforce\n2. Balanced\n3. Low Downforce\n4. Custom car\n5. Random car\n";
        if (!readInteger("Select a car: ", choice, 1, 5)) return false;
        Entry entry;
        if (choice <= 3) entry = presetEntry(choice);
        else if(choice==5) {
            double seedInput=0;
            if(advancedSettings && !readNumber("Random seed (0 generates a new car; otherwise 1 to 4294967295): ",seedInput))return false;
            if(seedInput<0 || seedInput>4294967295.0 || std::floor(seedInput)!=seedInput){std::cout<<"Seed must be a whole number from 0 to 4294967295.\n";return false;}
            std::uint32_t seed=static_cast<std::uint32_t>(seedInput);
            if(seed==0) {
                seed=input.autoSeed(number);
            }
            entry=randomEntry(seed,number);
            std::cout<<"Generated "<<entry.name<<"\nMass: "<<entry.car.mass<<" kg; wheel power: "<<entry.car.power
                <<" kW; Cd: "<<entry.car.dragCoefficient<<"; Cl: "<<entry.car.liftCoefficient<<"; area: "<<entry.car.frontalArea<<" m^2\n";
        }
        else
        {
            entry.car = presetEntry(2).car;
            entry.name = "Custom car " + std::to_string(number);
            entry.slug = "custom";
            if (!readNumber("Mass (kg): ", entry.car.mass)
                || !readNumber("Power at wheels (kW): ", entry.car.power)
                || !readNumber("Drag coefficient (Cd): ", entry.car.dragCoefficient)
                || !readNumber("Downforce coefficient (Cl): ", entry.car.liftCoefficient)
                || !readNumber("Frontal area (m^2): ", entry.car.frontalArea)) return false;
        }
        if (entry.car.mass <= 0 || entry.car.power <= 0 || entry.car.frontalArea <= 0
            || entry.car.dragCoefficient < 0 || entry.car.liftCoefficient < 0)
        {
            std::cout << "Mass, power and area must be positive; coefficients cannot be negative.\n";
            return false;
        }
        entries.push_back(std::move(entry));
        return true;
    };
    if (sessionChoice == 1)
    {
        if (!chooseCar(1)) return 1;
    }
    else
    {
        int selection = 1;
        std::cout << "\n1. Compare all three presets\n2. Choose 2 to 6 cars (presets or custom)\n";
        if (!readInteger("Select comparison: ", selection, 1, 2)) return 1;
        if (selection == 1)
            for (int i = 1; i <= 3; ++i) entries.push_back(presetEntry(i));
        else
        {
            int count = 2;
            if (!readInteger("Number of cars: ", count, 2, 6)) return 1;
            for (int i = 1; i <= count; ++i) if (!chooseCar(i)) return 1;
        }
    }
    Track track;

    int trackChoice = 3;

    std::cout << "\n--- TRACK SELECTION ---\n\n";
    std::cout << "1. Fictional preset tracks\n2. Design your own track\n3. Real F1 circuits\n";
    if (!readInteger("Select a track menu: ",trackChoice,1,3)) return 1;
    if(trackChoice==1) {
        int choice=1;
        std::cout << "\n1. Balanced circuit\n2. Low-speed / high-downforce circuit\n";
        if(!readInteger("Select a fictional preset: ",choice,1,2))return 1;
        track=presetTrack(choice);
    } else if(trackChoice==3) {
        int choice=1;
        std::vector<CircuitFile> circuits;
        try { circuits = discoverCircuits(trackDirectory); }
        catch (const std::exception& error) {
            std::cout << "Could not list circuits: " << error.what() << "\n";
            return 1;
        }
        std::cout << "\n--- REAL F1 CIRCUITS (" << circuits.size() << ") ---\n";
        for (std::size_t i = 0; i < circuits.size(); ++i)
            std::cout << i + 1 << ". " << circuits[i].name << "\n";
        if(!readInteger("Select a circuit: ", choice, 1, static_cast<int>(circuits.size())))return 1;
        try { track = loadTrackFile(circuits[static_cast<std::size_t>(choice - 1)].path); }
        catch(const std::exception& error){std::cout << "Could not load circuit: " << error.what() << "\n";return 1;}
        std::cout << "\nLoaded " << track.name << ".\n" << track.dataNote << "\n";
    }

    if (trackChoice == 2)
    {
        int numberOfSegments = 4;

        std::cout << "\n--- CUSTOM TRACK DESIGNER ---\n\n";
        std::cout << "How many track segments? ";
        if (!readInteger("", numberOfSegments, 1, 10000)) return 1;
        for (int i = 0; i < numberOfSegments; i++)
        {
            int segmentType = 1;

            std::cout << "\nSegment " << i + 1 << " type:\n";
            std::cout << "1. Straight\n";
            std::cout << "2. Corner\n";
            std::cout << "Select a type: ";
            if (!readInteger("", segmentType, 1, 2)) return 1;
            if (segmentType == 1)
            {
                double straightLength = 300.0;

                std::cout << "Straight length (m): ";
                if (!readNumber("", straightLength) || straightLength <= 0.0)
                {
                    std::cout << "Track dimensions must be positive.\n";
                    return 1;
                }

                track.addStraight(straightLength);
            }
            else if (segmentType == 2)
            {
                double cornerRadius = 60.0;
                double cornerAngle = 90.0;

                std::cout << "Corner radius (m): ";
                if (!readNumber("", cornerRadius) || cornerRadius <= 0.0)
                {
                    std::cout << "Track dimensions must be positive.\n";
                    return 1;
                }

                std::cout << "Corner angle (degrees): ";
                if (!readNumber("", cornerAngle) || cornerAngle <= 0.0)
                {
                    std::cout << "Track dimensions must be positive.\n";
                    return 1;
                }

                int direction=1;
                if (!readInteger("Turn direction (1 left, 2 right): ", direction, 1, 2)) return 1;
                track.addCorner(cornerRadius, cornerAngle, direction==1 ? 1 : -1);
            }
        }
    }

    double trackLength=0;
    for(const auto& s:track.segments)
        trackLength+=s.type==SegmentType::Straight ? s.length : s.radius*s.angle*3.14159265358979323846/180;
    std::cout << "\nTrack length: " << trackLength << " m\n";
    int sectorChoice=1;
    if (advancedSettings && !readInteger("Sectors (1 track defaults, 2 custom distances): ",sectorChoice,1,2)) return 1;
    if (sectorChoice==2) {
        track.sectorEnds={trackLength/3,2*trackLength/3};
        if (!readNumber("End of sector 1 (m from start): ",track.sectorEnds[0])
            || !readNumber("End of sector 2 (m from start): ",track.sectorEnds[1])) return 1;
        if (!(0<track.sectorEnds[0] && track.sectorEnds[0]<track.sectorEnds[1] && track.sectorEnds[1]<trackLength)) {
            std::cout << "Sector distances must satisfy 0 < S1 < S2 < track length.\n"; input.undo();
        }
    }
    int tractionChoice=1;
    if (advancedSettings && !readInteger("Traction setup (1 defaults, 2 customize each car): ",tractionChoice,1,2)) return 1;
    if (tractionChoice==2) for(auto& entry:entries) {
        std::cout << "\nTraction for " << entry.name << "\n";
        if (!readNumber("Rear weight/downforce fraction (0 to 1, exclusive): ",entry.car.rearFraction)
            || !readNumber("Tire load sensitivity (0 to 1, exclusive; default 0.10): ",entry.car.loadSensitivity)) return 1;
    }
    int elevationChoice=1;
    if(advancedSettings && !track.profile.empty() && !readInteger("Elevation physics (1 enabled, 2 disabled): ",elevationChoice,1,2))return 1;
    bool elevationEnabled=elevationChoice==1;
    const double airDensity = 1.225; // Reference density at sea level.
    // Maximum distance between calculation points (metres).
    const double distanceStep = 0.25;


    int lapChoice = 1;
    std::cout << "\n--- LAP / CONDITIONS (SHARED BY EVERY CAR) ---\n";
    std::cout << "1. Qualifying (flying lap)\n2. Race\n";
    if (!readInteger("Select a lap mode: ", lapChoice, 1, 2)) return 1;
    LapMode mode = LapMode::Qualifying;
    int raceLaps = 5; double rollingSpeedKmh = 80;
    if (lapChoice == 2) {
        int start = 1;
        std::cout << "1. Standing start\n2. Rolling start\n";
        if (!readInteger("Race start: ",start,1,2)) return 1;
        mode = start == 1 ? LapMode::RaceStanding : LapMode::RaceRolling;
        raceLaps = static_cast<int>(input.number("Race laps",5,1,20,true));
        if (start == 2) rollingSpeedKmh = input.number("Rolling start speed (km/h)",80,1,350);
    }
    double frictionCoefficient = 1.0;
    if (advancedSettings && (!readNumber("Tire friction coefficient at 2000 N per tire (mu): ", frictionCoefficient)
        || frictionCoefficient <= 0.0))
    {
        std::cout << "Grip must be positive.\n";
        return 1;
    }


    auto configuredEntries=entries;
    auto run=[&]() -> bool {
    auto entries = configuredEntries;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        auto& entry = entries[i];
        // Numeric prefixes keep repeated presets and custom cars distinct.
        entry.slug = std::to_string(i + 1) + "_" + entry.slug;
        entry.mu = frictionCoefficient;
        std::cout << "\nSimulating " << entry.name << "...\n";
        try
        {
            if (isRace(mode)) simulateRace(entry, track, frictionCoefficient, airDensity, distanceStep,
                                          mode, elevationEnabled, raceLaps, rollingSpeedKmh/3.6);
            else {
                entry.result = simulateTrack(entry.car, track, frictionCoefficient,
                                             airDensity, 0.0, distanceStep, mode, elevationEnabled);
                entry.sectors = sectorTimes(entry.result);
            }
        }
        catch (const std::exception& error)
        {
            std::cout << "Could not simulate " << entry.name << ": " << error.what() << "\n";
            return false;
        }
    }
    std::cout << "\nMap closure: " << (entries.front().result.closed ? "closed" : "open")
              << "; endpoint gap " << entries.front().result.closureGap << " m.\n";
    if (!entries.front().result.closed && mode != LapMode::SingleRun)
        std::cout << "Open geometry: lap planning repeats the supplied route across the finish.\n";
    if (entries.size() == 1 && !isRace(mode)) printSolo(entries.front(), track, airDensity);
    const std::size_t reference = fastestEntry(entries);
    if (isRace(mode)) {
        std::cout << "\nRace results / " << raceLaps << " laps\n";
        for (const auto& e:entries) std::cout << e.name << ": " << e.raceDuration << " s; gap " << e.raceDuration-entries[reference].raceDuration << " s\n";
    } else printComparison(entries, reference);

    std::filesystem::path sessionFolder;
    try
    {
        sessionFolder = createSessionFolder(outputRoot, entries.size() > 1, mode);
        saveSession(entries, track, frictionCoefficient, airDensity, distanceStep,
                    mode, reference, sessionFolder);
        std::cout << "\nSession saved to: " << sessionFolder.string() << "\n";
        std::cout << "Open: " << (sessionFolder / "session_report.html").string() << "\n";
        std::cout << "Earlier sessions have been kept.\n";
    }
    catch (const std::exception& error)
    {
        std::cout << "\nSimulation completed, but saving failed: " << error.what() << "\n";
        if (!sessionFolder.empty())
            std::cout << "Partial output may exist in: " << sessionFolder.string() << "\n";
        return false;
    }
    std::cout << "\nModel: inclined road, altitude-dependent air density, load-sensitive tire grip, constant wheel power.\n";

        return true;
    };
    bool newSetup=false;
    for(;;) {
        run();
        std::cout << "\n--- NEXT RUN ---\n1. Run again with the same setup\n2. Replace one car\n3. Change lap / conditions\n4. Start a new setup\n5. Exit\n";
        SetupInput next;
        int action=static_cast<int>(next.number("Select next action",5,1,5,true));
        if(action==5) return 0;
        if(action==4) { newSetup=true; break; }
        if(action==2) {
            SetupInput carSelection;
            int carIndex=static_cast<int>(carSelection.number("Car number to replace",1,1,static_cast<double>(configuredEntries.size()),true));
            // Car selection uses its own history so Back stays within this edit.
            auto originalEntries=entries;
            input=SetupInput{};
            for(;;) {
                input.replay();
                try { entries.clear(); chooseCar(carIndex); configuredEntries[static_cast<std::size_t>(carIndex-1)]=entries.front(); break; }
                catch(const BackInput&) { std::cout << "Returning to the previous car question.\n"; }
            }
            entries=originalEntries;
        }
        if(action==3) {
            SetupInput conditions;
            for(;;) {
                try {
                    conditions.replay();
                    int lap=static_cast<int>(conditions.number("Mode (1 qualifying, 2 race)",isRace(mode)?2:1,1,2,true));
                    LapMode nextMode=LapMode::Qualifying;int nextLaps=raceLaps;double nextSpeed=rollingSpeedKmh;
                    if(lap==2){
                        int start=static_cast<int>(conditions.number("Start (1 standing, 2 rolling)",mode==LapMode::RaceRolling?2:1,1,2,true));
                        nextMode=start==1?LapMode::RaceStanding:LapMode::RaceRolling;
                        nextLaps=static_cast<int>(conditions.number("Race laps",raceLaps,1,20,true));
                        if(start==2)nextSpeed=conditions.number("Rolling start speed (km/h)",rollingSpeedKmh,1,350);
                    }
                    double grip=conditions.number("Tire grip mu",frictionCoefficient,0.000001,100);
                    int elevation=static_cast<int>(conditions.number("Elevation (1 enabled, 2 disabled)",elevationEnabled ? 1 : 2,1,2,true));
                    mode=nextMode;raceLaps=nextLaps;rollingSpeedKmh=nextSpeed;
                    frictionCoefficient=grip; elevationEnabled=elevation==1;
                    break;
                } catch(const BackInput&) { std::cout << "Returning to the previous conditions question.\n"; }
            }
        }
    }
    if(newSetup) break;
    } catch(const BackInput&) { std::cout << "\nReturning to the previous question; earlier answers are retained.\n"; }
    }
    }
    } catch(const EndInput&) { std::cout << "\nSimulator closed.\n"; return 0; }
    catch(const std::exception& error) { std::cout << "\n" << error.what() << "\n"; return 1; }
}
