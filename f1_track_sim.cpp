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
    auto rank = [](const std::filesystem::path& path) {
        const auto name = path.filename().string();
        return name == "monza.track" ? 0 : name == "spa.track" ? 1 : name == "cota.track" ? 2 : 3;
    };
    std::sort(circuits.begin(), circuits.end(), [&](const CircuitFile& a, const CircuitFile& b) {
        const int ar = rank(a.path), br = rank(b.path);
        if (ar != br) return ar < br;
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

enum class LapMode { SingleRun, StandingStart, Qualifying };

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

    if (mode == LapMode::StandingStart) startSpeed = 0.0;
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
    if (mode != LapMode::SingleRun)
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
        if(dragFactor>0 && mode!=LapMode::SingleRun) {
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
    if (mode == LapMode::SingleRun) backwardPass();
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
    out << "];\nconst mapClosed=" << (result.closed?"true":"false")
        << ",closureGap=" << result.closureGap << ";\n";
}

void exportTelemetry(const Car& car, const Simulation& result, double mu,
                     double density, LapMode mode,
                     const std::string& htmlPath = "lap_report.html",
                     const std::string& csvPath = "lap_telemetry.csv")
{
    (void)mu;
    std::ofstream html(htmlPath), csv(csvPath);
    if (!html || !csv) throw std::runtime_error("Could not create the report files.");
    html.imbue(std::locale::classic()); csv.imbue(std::locale::classic());
    html << std::setprecision(15); csv << std::setprecision(15);
    const auto sectors = sectorTimes(result);
    const char* modeName = mode == LapMode::Qualifying ? "Qualifying lap" : "Standing start";
    html << R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lap telemetry</title><style>
:root{color-scheme:dark;--bg:#101719;--panel:#192326;--line:#314044;--muted:#a4b4b9;--ink:#edf6f3;--teal:#64e3bc;--coral:#ff887b;--gold:#efce7c}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px system-ui,Segoe UI,sans-serif}main{max-width:1200px;margin:auto;padding:38px 26px}header{display:flex;align-items:end;justify-content:space-between;gap:20px}.eyebrow{font-size:12px;letter-spacing:.16em;color:var(--teal);text-transform:uppercase}h1{font-size:clamp(28px,4vw,44px);letter-spacing:-.04em;margin:8px 0}p{color:var(--muted);line-height:1.6}.pill{border:1px solid var(--line);border-radius:30px;padding:9px 15px;white-space:nowrap}.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:25px 0}.stat{background:var(--panel);padding:19px;border:1px solid var(--line);border-radius:12px}.stat span{display:block;color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.07em}.stat strong{display:block;font-size:27px;margin:8px 0 3px;font-weight:600}.stat small{color:var(--muted)}.total{border-top:3px solid var(--teal)}.toolbar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:28px 0 14px}button{background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:7px;padding:9px 14px;cursor:pointer}button:hover{border-color:var(--teal)}.readout{display:grid;grid-template-columns:repeat(5,1fr);gap:8px;font-variant-numeric:tabular-nums;margin:0 0 16px}.readout div{padding:12px;background:var(--panel);border-radius:8px}.readout small{display:block;color:var(--muted);font-size:11px;margin-bottom:5px}.readout b{font-size:15px}.chart{background:var(--panel);border:1px solid var(--line);border-radius:12px;margin:12px 0;padding:16px 16px 8px}.charthead{display:flex;flex-wrap:wrap;justify-content:space-between;gap:10px}h2{font-size:15px;margin:0}.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:12px;color:var(--muted)}.legend i{display:inline-block;width:14px;height:3px;margin-right:6px;vertical-align:middle}canvas{display:block;width:100%;height:205px;touch-action:pan-y}input[type=range]{width:100%;accent-color:var(--teal)}.hint{font-size:12px;margin:8px 0}.foot{border-top:1px solid var(--line);margin-top:24px;padding-top:16px;font-size:12px}a{color:var(--teal)}@media(max-width:650px){main{padding:22px 14px}.stats{grid-template-columns:repeat(2,1fr)}.readout{grid-template-columns:repeat(2,1fr)}header{display:block}.pill{display:inline-block;margin-top:10px}.toolbar{align-items:start}canvas{height:190px}.stat strong{font-size:23px}}
.telemetry-layout{display:grid;grid-template-columns:minmax(300px,.9fr) minmax(0,1.3fr);gap:20px;align-items:start;margin:24px 0}.map-panel{position:sticky;top:16px;background:var(--bg,#101719);border-radius:12px}.map-panel .chart{margin:0}.graph-panel{min-width:0}.graph-tabs{display:flex;gap:7px;overflow:auto;padding:4px 0 12px}.graph-tabs button{white-space:nowrap}.graph-tabs button[aria-pressed=true]{color:#64e3bc;border-color:#64e3bc}.graph-view[hidden]{display:none}.map-panel select{background:#192326;color:#edf6f3;border:1px solid #314044;border-radius:6px;padding:5px}.map-panel #mapNote{font-size:11px;line-height:1.4;max-height:48px;overflow:auto}.graph-panel canvas{height:290px}.map-panel #trackMap{height:340px!important}.graph-panel .readout{grid-template-columns:repeat(2,1fr)}@media(max-width:800px){.telemetry-layout{grid-template-columns:1fr;gap:12px}.map-panel{top:0;z-index:3;border-bottom:1px solid #314044;padding-bottom:8px}.map-panel #trackMap{height:180px!important}.map-panel #mapNote{display:none}.map-panel .chart{padding:10px}.graph-panel canvas{height:220px}.map-panel h2{font-size:13px}.map-panel label,.map-panel button,.map-panel select{font-size:12px}.map-panel #playbackStatus{display:none}}
</style></head><body><main><header><div><div class="eyebrow">Lap time simulator / telemetry</div><h1>Every metre of the lap.</h1><p id="subtitle"></p></div><div class="pill" id="mode"></div></header>


<section class="stats" id="stats" aria-label="Lap and sector times"></section>
<div class="toolbar"><div><h2>Explore the lap</h2><p class="hint">Hover or tap a graph. Use the slider for precise inspection.</p></div><button id="reset">Reset view</button></div>


<div class="telemetry-layout"><aside class="map-panel"><section class="chart"><h2>2D track map</h2><p id="mapNote"></p><div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:14px 0"><button id="play" aria-pressed="false">Play lap</button><label><input id="loop" type="checkbox" checked> Loop</label><label>Playback <select id="playbackSpeed"><option value="0.25">0.25x slow-mo</option><option value="0.5">0.5x slow-mo</option><option value="1">1x</option><option value="4" selected>4x</option><option value="8">8x</option><option value="16">16x</option></select></label><label>Map orientation <select id="orientation"><option value="typical">Circuit diagram</option><option value="north">North up</option><option value="90">Rotate 90°</option><option value="180">Rotate 180°</option><option value="270">Rotate 270°</option></select></label><span id="playbackStatus" style="color:#a4b4b9">Paused</span></div><div style="display:flex;gap:8px;align-items:center;margin:8px 0"><button id="zoomOut" aria-label="Zoom out">−</button><span id="zoomLevel">100%</span><button id="zoomIn" aria-label="Zoom in">+</button><button id="zoomReset">Fit track</button><label>Track car <select id="followCar"><option value="none">Off</option><option value="0">Follow car</option></select></label><small style="color:#a4b4b9">Scroll to zoom · drag to pan</small></div><canvas id="trackMap" style="height:320px" aria-label="Track map with sector markers and selected position"></canvas></section></aside><div class="graph-panel"><nav class="graph-tabs" aria-label="Telemetry view"><button data-view="speed" aria-pressed="true">Speed</button><button data-view="accel" aria-pressed="false">Acceleration</button><button data-view="force" aria-pressed="false">Forces</button><button data-view="elevation" aria-pressed="false">Elevation</button></nav><section class="readout" aria-live="polite"><div><small>POSITION / SECTOR</small><b id="position"></b></div><div><small>SPEED</small><b id="speed"></b></div><div><small>LONGITUDINAL / LATERAL</small><b id="g"></b></div><div><small>DRIVE / BRAKE FORCE</small><b id="force"></b></div><div><small>ELAPSED / SEGMENT</small><b id="elapsed"></b></div></section><input id="scrub" type="range" min="0" max="10000" value="0" aria-label="Position around lap"><section class="chart graph-view" data-graph="speed"><div class="charthead"><h2>Speed <span style="color:var(--muted)">/ km/h</span></h2><div class="legend"><span><i style="background:var(--teal)"></i>Vehicle speed</span></div></div><canvas id="speedChart" aria-label="Speed versus distance"></canvas></section>
<section class="chart graph-view" data-graph="accel" hidden><div class="charthead"><h2>Acceleration <span style="color:var(--muted)">/ g</span></h2><div class="legend"><span><i style="background:var(--teal)"></i>Longitudinal (+ accelerate, − slow)</span><span><i style="background:var(--gold)"></i>Lateral magnitude</span></div></div><canvas id="accelChart" aria-label="Longitudinal and lateral acceleration versus distance"></canvas></section>
<section class="chart graph-view" data-graph="force" hidden><div class="charthead"><h2>Applied tire force <span style="color:var(--muted)">/ kN</span></h2><div class="legend"><span><i style="background:var(--teal)"></i>Driving</span><span><i style="background:var(--coral)"></i>Braking</span></div></div><canvas id="forceChart" aria-label="Driving and braking forces versus distance"></canvas></section>
<section class="chart graph-view" data-graph="elevation" hidden><h2>Elevation / metres above sea level</h2><p id="elevationReadout"></p><canvas id="elevationChart" aria-label="Track elevation profile"></canvas></section></div></div>
<p class="hint">Distance along track (m). Dashed markers divide the configured sectors. Brake force excludes aerodynamic drag; slowing down does not always mean braking.</p>
<div class="foot"><p>Modeled wheel forces, not pedal positions. Inclined road, altitude-dependent air density, load-sensitive tire grip and constant available wheel power; no gears, tire temperatures or load transfer. The final segment connects to the first for speed planning; map closure is checked for endpoint position and heading.</p><p><a href="lap_telemetry.csv" download>Download full telemetry CSV</a> · Values in the CSV are sampled at cell midpoints. This report works offline. Geometry: <a href="https://github.com/bacinger/f1-circuits">bacinger/f1-circuits</a> (MIT). Elevation: <a href="https://www.opentopodata.org/datasets/srtm/">SRTM90m / Open Topo Data</a>.</p></div>
</main><script>
'use strict';
const lap={mode:")HTML" << modeName << "\",length:" << result.totalDistance
         << ",time:" << result.totalTime << ",mass:" << car.mass
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
$('mode').textContent=lap.mode;
$('subtitle').textContent=`${trackName} · ${fmt(lap.length,1)} m circuit · ${cells.at(-1)[8]} segments · three sectors`;
$('stats').innerHTML=[['Lap time',lap.time,`${lap.mode} · M:SS.mss`],...lap.sectors.map((t,i)=>[`Sector ${i+1}`,t,`${fmt(bounds[i],0)}–${fmt(bounds[i+1],0)} m`])].map((s,i)=>`<div class="stat ${i===0?'total':''}"><span>${s[0]}</span><strong>${i===0?formatLapTime(s[1]):fmt(s[1],3)} ${i===0?'':'<small>s</small>'}</strong><small>${s[2]}</small></div>`).join('');
function sample(c,x){let d=Math.max(0,Math.min(c[1]-c[0],x-c[0])),v=Math.sqrt(Math.max(0,c[4]**2+2*c[6]*d)),t=c[2]+(d?2*d/(c[4]+v):0),f=lap.mass*(c[6]+9.81*c[9]/Math.sqrt(1+c[9]*c[9]))+c[10]*v*v;return {x,v:v*3.6,a:c[6]/9.81,l:v*v*c[7]/9.81,drive:Math.max(0,f)/1000,brake:Math.max(0,-f)/1000,t,segment:c[8]};}
function at(x){let lo=0,hi=cells.length-1;while(lo<hi){let m=(lo+hi)>>1;if(cells[m][1]<=x)lo=m+1;else hi=m;}return sample(cells[lo],x);}
// Preserve extrema when displaying large data sets. Hover always uses full data.
const points=[];let stride=Math.max(1,Math.ceil(cells.length/2500));
for(let i=0;i<cells.length;i+=stride){const candidates=[];for(let j=i;j<Math.min(cells.length,i+stride);j++)candidates.push(sample(cells[j],cells[j][0]),sample(cells[j],cells[j][1]));const keep=new Set([0,candidates.length-1]);for(const key of ['v','a','l','drive','brake']){let lo=0,hi=0;for(let k=1;k<candidates.length;k++){if(candidates[k][key]<candidates[lo][key])lo=k;if(candidates[k][key]>candidates[hi][key])hi=k;}keep.add(lo);keep.add(hi);}for(const k of [...keep].sort((a,b)=>a-b))points.push(candidates[k]);}
const charts=[{id:'speedChart',keys:['v'],colors:['#64e3bc']},{id:'accelChart',keys:['a','l'],colors:['#64e3bc','#efce7c']},{id:'forceChart',keys:['drive','brake'],colors:['#64e3bc','#ff887b']}];
let cursor=0;
for(const ch of charts){let low=0,high=0;for(const p of points)for(const k of ch.keys){low=Math.min(low,p[k]);high=Math.max(high,p[k]);}const pad=Math.max(.1,(high-low)*.12);ch.low=low<0?low-pad:0;ch.high=high+pad;}
function draw(ch){const canvas=$(ch.id);if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight,dpr=window.devicePixelRatio||1;const ctx=prepareCanvas(canvas);const left=49,right=w-15,top=28,bottom=h-30;ch.left=left;ch.right=right;const px=x=>left+x/lap.length*(right-left),py=y=>bottom-(y-ch.low)/(ch.high-ch.low)*(bottom-top);ctx.font='11px system-ui';ctx.lineWidth=1;
for(let i=0;i<3;i++){ctx.fillStyle=i%2?'#ffffff04':'#ffffff08';ctx.fillRect(px(bounds[i]),top,px(bounds[i+1])-px(bounds[i]),bottom-top);ctx.fillStyle='#a4b4b9';ctx.textAlign='center';ctx.fillText(`S${i+1}`,px((bounds[i]+bounds[i+1])/2),16);}
for(let i=0;i<=4;i++){const v=ch.low+(ch.high-ch.low)*i/4,y=py(v);ctx.strokeStyle='#314044';ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();ctx.textAlign='right';ctx.fillStyle='#a4b4b9';ctx.fillText(fmt(v,ch.id==='speedChart'?0:1),left-8,y+4);}
for(let i=0;i<=3;i++){let x=px(bounds[i]);ctx.setLineDash([4,5]);ctx.strokeStyle='#536268';ctx.beginPath();ctx.moveTo(x,top);ctx.lineTo(x,bottom);ctx.stroke();ctx.setLineDash([]);ctx.textAlign=i===0?'left':i===3?'right':'center';ctx.fillStyle='#a4b4b9';ctx.fillText(fmt(bounds[i],0)+' m',x,h-9);}
ch.keys.forEach((k,n)=>{ctx.beginPath();ctx.strokeStyle=ch.colors[n];ctx.lineWidth=1.7;points.forEach((p,i)=>{const x=px(p.x),y=py(p[k]);if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});ctx.stroke();});
const p=at(cursor);ctx.strokeStyle='#edf6f3';ctx.lineWidth=1;ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(px(cursor),top);ctx.lineTo(px(cursor),bottom);ctx.stroke();ctx.setLineDash([]);ch.keys.forEach((k,n)=>{ctx.fillStyle=ch.colors[n];ctx.beginPath();ctx.arc(px(cursor),py(p[k]),3,0,Math.PI*2);ctx.fill();});}


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
function installMapZoom(){$('followCar').addEventListener('change',()=>redrawMapView());const canvas=$('trackMap');canvas.style.touchAction='none';$('zoomIn').addEventListener('click',()=>zoomMap(mapZoom*1.3));$('zoomOut').addEventListener('click',()=>zoomMap(mapZoom/1.3));$('zoomReset').addEventListener('click',()=>{mapZoom=1;mapPanX=mapPanY=0;$('followCar').value='none';redrawMapView();});canvas.addEventListener('wheel',e=>{e.preventDefault();const rect=canvas.getBoundingClientRect();zoomMap(mapZoom*Math.exp(-e.deltaY*.0015),e.clientX-rect.left-canvas.clientWidth/2,e.clientY-rect.top-canvas.clientHeight/2);},{passive:false});canvas.addEventListener('pointerdown',e=>{$('followCar').value='none';mapDrag={id:e.pointerId,x:e.clientX,y:e.clientY};canvas.setPointerCapture(e.pointerId);});canvas.addEventListener('pointermove',e=>{if(!mapDrag||e.pointerId!==mapDrag.id)return;mapPanX+=e.clientX-mapDrag.x;mapPanY+=e.clientY-mapDrag.y;mapDrag.x=e.clientX;mapDrag.y=e.clientY;redrawMapView();});const stop=e=>{if(mapDrag&&e.pointerId===mapDrag.id)mapDrag=null;};canvas.addEventListener('pointerup',stop);canvas.addEventListener('pointercancel',stop);canvas.addEventListener('lostpointercapture',stop);}
function installPlayback(){installMapZoom();document.querySelectorAll('[data-view]').forEach(button=>button.addEventListener('click',()=>{document.querySelectorAll('[data-view]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));document.querySelectorAll('[data-graph]').forEach(panel=>panel.hidden=panel.dataset.graph!==button.dataset.view);renderVersion++;update(cursor);}));$('play').addEventListener('click',()=>setPlaying(!playing));$('orientation').addEventListener('change',e=>{mapRotation=e.target.value==='typical'?defaultMapRotation:e.target.value==='north'?0:defaultMapRotation+Number(e.target.value);renderVersion++;update(cursor);});$('scrub').addEventListener('pointerdown',()=>setPlaying(false));$('scrub').addEventListener('keydown',()=>setPlaying(false));$('reset').addEventListener('click',()=>{setPlaying(false);playbackTime=0;update(0);});}

function sectorAtDistance(x){return x<bounds[1]?1:x<bounds[2]?2:3;}
function mapAt(d){let lo=0,hi=trackMap.length-1;while(lo<hi){let m=(lo+hi)>>1;if(trackMap[m][0]<d)lo=m+1;else hi=m;}if(!lo)return trackMap[0];const a=trackMap[lo-1],b=trackMap[lo],t=Math.max(0,Math.min(1,(d-a[0])/(b[0]-a[0])));return [d,a[1]+t*(b[1]-a[1]),a[2]+t*(b[2]-a[2])];}
function drawElevation(d){const canvas=$('elevationChart');if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight;let low=Infinity,high=-Infinity;for(const p of elevationProfile){low=Math.min(low,p[1]);high=Math.max(high,p[1]);}const pad=Math.max(2,(high-low)*.15);low-=pad;high+=pad;const px=x=>50+x/bounds[3]*(w-70),py=y=>h-30-(y-low)/(high-low)*(h-55);
const ctx=cachedBackground(canvas,'elevation',bg=>{bg.font='11px system-ui';bg.fillStyle='#adbec3';for(let i=0;i<5;i++){const y=low+(high-low)*i/4;bg.fillText(y.toFixed(0)+' m',2,py(y));bg.strokeStyle='#314044';bg.beginPath();bg.moveTo(50,py(y));bg.lineTo(w-20,py(y));bg.stroke();}bg.strokeStyle='#efce7c';bg.lineWidth=2;bg.beginPath();elevationProfile.forEach((p,i)=>i?bg.lineTo(px(p[0]),py(p[1])):bg.moveTo(px(p[0]),py(p[1])));bg.stroke();});ctx.strokeStyle='#edf6f3';ctx.beginPath();ctx.moveTo(px(d),20);ctx.lineTo(px(d),h-30);ctx.stroke();let lo=0,hi=elevationProfile.length-1;while(lo<hi){const m=(lo+hi)>>1;if(elevationProfile[m][0]<d)lo=m+1;else hi=m;}const p=elevationProfile[lo];$('elevationReadout').textContent=p[1].toFixed(1)+' m altitude · '+(p[2]*100).toFixed(1)+'% gradient · '+(elevationEnabled?'Elevation physics enabled':'Elevation physics disabled')+'. Terrain estimate; not a surveyed road profile.';}
function drawMap(d){const canvas=$('trackMap'),w=canvas.clientWidth,h=canvas.clientHeight;if(!rotatedGeometry||rotatedGeometry.rotation!==mapRotation){const points=trackMap.map(rotatePoint);let xmin=Infinity,xmax=-Infinity,ymin=Infinity,ymax=-Infinity;for(const p of points){xmin=Math.min(xmin,p[1]);xmax=Math.max(xmax,p[1]);ymin=Math.min(ymin,p[2]);ymax=Math.max(ymax,p[2]);}rotatedGeometry={rotation:mapRotation,points,xmin,xmax,ymin,ymax};}const {points:rotated,xmin,xmax,ymin,ymax}=rotatedGeometry;const scale=mapZoom*Math.min((w-80)/Math.max(1,xmax-xmin),(h-70)/Math.max(1,ymax-ymin)),px=x=>w/2+mapPanX+(x-(xmin+xmax)/2)*scale,py=y=>h/2+mapPanY-(y-(ymin+ymax)/2)*scale;
if($('followCar').value!=='none'){const target=rotatePoint(mapAt(d));mapPanX=-(target[1]-(xmin+xmax)/2)*scale;mapPanY=(target[2]-(ymin+ymax)/2)*scale;}
const ctx=cachedBackground(canvas,'map'+renderVersion+':'+mapPanX+':'+mapPanY,bg=>{const colors=['#64e3bc','#efce7c','#ff887b'];bg.lineWidth=5;bg.lineCap='round';for(let s=0;s<3;s++){const ps=[rotatePoint(mapAt(bounds[s])),...rotated.filter(p=>p[0]>bounds[s]&&p[0]<bounds[s+1]),rotatePoint(mapAt(bounds[s+1]))];bg.strokeStyle=colors[s];bg.beginPath();ps.forEach((p,i)=>i?bg.lineTo(px(p[1]),py(p[2])):bg.moveTo(px(p[1]),py(p[2])));bg.stroke();}bg.font='11px system-ui';bg.fillStyle='#edf6f3';turnLabels.map(rotatePoint).forEach((p,i)=>bg.fillText('T'+(i+1),px(p[1])+(i%2?-23:7),py(p[2])+(i%2?12:-9)));for(let i=0;i<4;i++){const p=rotatePoint(mapAt(bounds[i]));bg.fillRect(px(p[1])-3,py(p[2])-3,6,6);bg.fillText(i===0?'Start':i===3?'Finish':'S'+i,px(p[1])+8,py(p[2])+14+(i===3&&mapClosed?12:0));}});
const p=rotatePoint(mapAt(d)),ahead=rotatePoint(mapAt(Math.min(bounds[3],d+3))),behind=rotatePoint(mapAt(Math.max(0,d-3)));const heading=Math.atan2(py(ahead[2])-py(behind[2]),px(ahead[1])-px(behind[1]));ctx.save();ctx.translate(px(p[1]),py(p[2]));ctx.rotate(heading);ctx.fillStyle='#ffffff';ctx.strokeStyle='#101719';ctx.lineWidth=1.5;ctx.beginPath();ctx.moveTo(9,0);ctx.lineTo(-6,-5);ctx.lineTo(-3,0);ctx.lineTo(-6,5);ctx.closePath();ctx.fill();ctx.stroke();ctx.restore();$('mapNote').textContent='S1 green · S2 gold · S3 coral · '+(mapClosed?'Closed circuit':'Open route; endpoint gap '+closureGap.toFixed(1)+' m')+'. '+dataNote;drawElevation(d);}

function elapsedAt(d){return at(d).t;}function lapDuration(){return lap.time;}
function distanceAtTime(t){let lo=0,hi=cells.length-1;while(lo<hi){const m=(lo+hi)>>1;if(cells[m][3]<=t)lo=m+1;else hi=m;}const c=cells[lo],dt=Math.max(0,Math.min(c[3]-c[2],t-c[2]));return Math.min(c[1],c[0]+c[4]*dt+.5*c[6]*dt*dt);}
function update(x){cursor=Math.max(0,Math.min(lap.length,x));const p=at(cursor);$('scrub').value=cursor/lap.length*10000;$('position').textContent=`${fmt(cursor,1)} m / S${sectorAtDistance(cursor)}`;$('speed').textContent=fmt(p.v,1)+' km/h';$('g').textContent=`${fmt(p.a)} / ${fmt(p.l)} g`;$('force').textContent=`${fmt(p.drive)} / ${fmt(p.brake)} kN`;$('elapsed').textContent=`${fmt(p.t,3)} s / ${p.segment}`;charts.forEach(draw);drawMap(cursor);}
for(const ch of charts){$(ch.id).addEventListener('pointermove',e=>{if(playing)return;const rect=e.currentTarget.getBoundingClientRect();update((e.clientX-rect.left-ch.left)/(ch.right-ch.left)*lap.length);});$(ch.id).addEventListener('pointerdown',e=>{setPlaying(false);const rect=e.currentTarget.getBoundingClientRect();update((e.clientX-rect.left-ch.left)/(ch.right-ch.left)*lap.length);});}
$('scrub').addEventListener('input',e=>{setPlaying(false);update(Number(e.target.value)/10000*lap.length);});$('reset').addEventListener('click',()=>update(0));window.addEventListener('resize',()=>update(cursor));installPlayback();update(0);
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
        [](const Entry& a, const Entry& b) { return a.result.totalTime < b.result.totalTime; }) - entries.begin());
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
          << (mode == LapMode::Qualifying ? "_qualifying" : "_standing");
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
    const char* modeName = mode == LapMode::Qualifying ? "qualifying" : "standing_start";
    std::ofstream summary(folder / "summary.csv"), gaps(folder / "time_gaps.csv"), setup(folder / "session.json");
    if (!summary || !gaps || !setup) throw std::runtime_error("Could not create session summary files.");
    for (auto* out : {&summary, &gaps, &setup}) { out->imbue(std::locale::classic()); *out << std::setprecision(15); }
    summary << "car_id,car_name,lap_mode,lap_time_s,delta_to_reference_s,sector_1_s,sector_2_s,sector_3_s,sector_1_delta_s,sector_2_delta_s,sector_3_delta_s,top_speed_km_h,reference_car_id\n";
    setup << "{\n\"format_version\":3,\"session\":" << jsonString(folder.filename().string())
          << ",\"lap_mode\":" << jsonString(modeName) << ",\"mu\":" << mu
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
                        (carFolder / "lap_report.html").string(), (carFolder / "lap_telemetry.csv").string());
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
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Lap session report</title>
<style>
:root{color-scheme:dark;--bg:#101719;--panel:#192326;--line:#314044;--muted:#adbec3;--ink:#edf6f3;--mint:#64e3bc}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px system-ui,Segoe UI,sans-serif}main{max-width:1240px;margin:auto;padding:38px 26px 55px}.eyebrow{color:var(--mint);font-size:11px;letter-spacing:.16em;text-transform:uppercase}header{display:flex;justify-content:space-between;align-items:center;gap:24px}h1{font-size:clamp(29px,4vw,46px);letter-spacing:-.04em;margin:10px 0}h2{font-size:17px;margin:0}p{color:var(--muted);line-height:1.65}.badge{border:1px solid var(--line);border-radius:30px;padding:10px 15px;white-space:nowrap;font-size:13px}.summary{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:22px 0}.summary>div{border:1px solid var(--line);background:var(--panel);border-radius:11px;padding:20px}.summary small{display:block;color:var(--muted);font-size:12px}.summary strong{display:block;font-size:26px;margin:8px 0}.muted{color:var(--muted)}section{margin-top:26px}.bar{display:flex;justify-content:space-between;align-items:center;gap:14px;flex-wrap:wrap}.bar p{margin:6px 0 0;font-size:13px}select,button{background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:7px;padding:9px;font:inherit;font-size:13px}label{font-size:13px}a{color:var(--mint);text-underline-offset:3px}.tablewrap{overflow-x:auto;border:1px solid var(--line);border-radius:11px;margin-top:15px;background:var(--panel)}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;white-space:nowrap}th,td{padding:15px 13px;text-align:right;border-bottom:1px solid var(--line)}th{font-size:11px;color:var(--muted);font-weight:500;text-transform:uppercase;letter-spacing:.05em}th:first-child,td:first-child{text-align:left}tbody tr:last-child td{border-bottom:0}td small{display:block;color:var(--muted);font-size:11px;margin-top:4px}.dot{width:9px;height:9px;border-radius:50%;display:inline-block;margin-right:8px}.best{color:var(--mint)}.legend{display:flex;flex-wrap:wrap;gap:10px 20px;margin:18px 0}.legend label{display:flex;align-items:center;gap:5px;cursor:pointer}.legend input{accent-color:var(--mint)}.chart{border:1px solid var(--line);border-radius:11px;background:var(--panel);padding:17px 15px 5px;margin-top:14px}.chart h2{font-size:15px}.chart p{font-size:12px;margin:6px 0}canvas{width:100%;height:245px;display:block;touch-action:pan-y}.cursorbar{display:flex;justify-content:space-between;align-items:center;font-size:13px;color:var(--muted);gap:12px;margin-top:20px}input[type=range]{width:100%;accent-color:var(--mint);margin:15px 0 0}.inspect td,.inspect th{padding:11px 13px}.links{display:flex;gap:18px;flex-wrap:wrap;font-size:13px;margin-top:18px}details{border:1px solid var(--line);border-radius:10px;padding:15px;margin-top:20px}summary{cursor:pointer}footer{border-top:1px solid var(--line);margin-top:30px;padding-top:12px;font-size:12px}button{cursor:pointer}.note{font-size:12px} @media(max-width:650px){main{padding:23px 14px}.summary{grid-template-columns:1fr}.summary>div{padding:13px 17px}.summary strong{font-size:22px}header{display:block}.badge{display:inline-block;margin-top:5px}canvas{height:215px}th,td{padding:12px 10px}}
.telemetry-layout{display:grid;grid-template-columns:minmax(300px,.9fr) minmax(0,1.3fr);gap:20px;align-items:start;margin:24px 0}.map-panel{position:sticky;top:16px;background:var(--bg,#101719);border-radius:12px}.map-panel .chart{margin:0}.graph-panel{min-width:0}.graph-tabs{display:flex;gap:7px;overflow:auto;padding:4px 0 12px}.graph-tabs button{white-space:nowrap}.graph-tabs button[aria-pressed=true]{color:#64e3bc;border-color:#64e3bc}.graph-view[hidden]{display:none}.map-panel select{background:#192326;color:#edf6f3;border:1px solid #314044;border-radius:6px;padding:5px}.map-panel #mapNote{font-size:11px;line-height:1.4;max-height:48px;overflow:auto}.graph-panel canvas{height:290px}.map-panel #trackMap{height:340px!important}.graph-panel .readout{grid-template-columns:repeat(2,1fr)}@media(max-width:800px){.telemetry-layout{grid-template-columns:1fr;gap:12px}.map-panel{top:0;z-index:3;border-bottom:1px solid #314044;padding-bottom:8px}.map-panel #trackMap{height:180px!important}.map-panel #mapNote{display:none}.map-panel .chart{padding:10px}.graph-panel canvas{height:220px}.map-panel h2{font-size:13px}.map-panel label,.map-panel button,.map-panel select{font-size:12px}.map-panel #playbackStatus{display:none}}
</style></head><body><main>
<header><div><div class="eyebrow">Lap time simulator / session archive</div><h1 id="heading"></h1><p id="subtitle"></p></div><div class="badge" id="mode"></div></header>
<div class="summary"><div><small id="winnerLabel">FASTEST LAP</small><strong id="winnerTime"></strong><small id="winnerName"></small></div><div><small>TRACK DISTANCE</small><strong id="length"></strong><small>Configured sector boundaries</small></div><div><small>START / FINISH</small><strong id="startMode"></strong><small id="startNote"></small></div></div>
<section><div class="bar"><div><h2>Lap &amp; sector results</h2><p>Differences use the selected reference. Negative means faster; positive means slower.</p></div><label>Reference <select id="reference" aria-label="Reference car"></select></label></div>
<div class="tablewrap"><table><thead><tr><th>Car</th><th>Lap / difference</th><th>Sector 1 / difference</th><th>Sector 2 / difference</th><th>Sector 3 / difference</th><th>Top speed</th><th>Telemetry</th></tr></thead><tbody id="results"></tbody></table></div></section>


<section><div class="bar"><div><h2>Compare at the same point on track</h2><p>Toggle cars below. Hover or tap a graph, or use the position slider.</p></div><button id="reset">Reset position</button></div>
<div class="telemetry-layout"><aside class="map-panel"><section class="chart"><h2>2D track map</h2><p id="mapNote"></p><div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:14px 0"><button id="play" aria-pressed="false">Play lap</button><label><input id="loop" type="checkbox" checked> Loop</label><label>Playback <select id="playbackSpeed"><option value="0.25">0.25x slow-mo</option><option value="0.5">0.5x slow-mo</option><option value="1">1x</option><option value="4" selected>4x</option><option value="8">8x</option><option value="16">16x</option></select></label><label>Map orientation <select id="orientation"><option value="typical">Circuit diagram</option><option value="north">North up</option><option value="90">Rotate 90°</option><option value="180">Rotate 180°</option><option value="270">Rotate 270°</option></select></label><span id="playbackStatus" style="color:#a4b4b9">Paused</span></div><div style="display:flex;gap:8px;align-items:center;margin:8px 0"><button id="zoomOut" aria-label="Zoom out">−</button><span id="zoomLevel">100%</span><button id="zoomIn" aria-label="Zoom in">+</button><button id="zoomReset">Fit track</button><label>Track car <select id="followCar"><option value="none">Off</option><option value="0">Follow car</option></select></label><small style="color:#a4b4b9">Scroll to zoom · drag to pan</small></div><canvas id="trackMap" style="height:320px" aria-label="Track map with sector markers and selected position"></canvas></section></aside><div class="graph-panel"><nav class="graph-tabs" aria-label="Telemetry view"><button data-view="speed" aria-pressed="true">Speed</button><button data-view="gap" aria-pressed="false">Time gap</button><button data-view="elevation" aria-pressed="false">Elevation</button></nav><div class="chart graph-view" data-graph="speed"><h2>Speed <span class="muted">/ km/h</span></h2><p>Check the cars you want to compare. Uncheck a car to hide its trace and map marker.</p><fieldset style="border:1px solid #314044;border-radius:8px;padding:8px 12px;margin:12px 0"><legend style="color:#a4b4b9;font-size:12px">Cars to compare</legend><div class="legend" id="legend"></div></fieldset><canvas id="speedChart" aria-label="Car speed comparison versus distance"></canvas></div>
<div class="chart graph-view" data-graph="gap" hidden><h2>Time gap <span class="muted">/ seconds</span></h2><p id="gapNote"></p><canvas id="gapChart" aria-label="Elapsed time difference from reference versus distance"></canvas></div>
<div class="cursorbar"><strong id="position"></strong><span>Distance along track (m)</span></div><input id="scrub" type="range" min="0" max="10000" value="0" aria-label="Position along track">
<div class="tablewrap"><table class="inspect"><thead><tr><th>At this position</th><th>Speed</th><th>Elapsed time</th><th>Gap to reference</th></tr></thead><tbody id="inspection"></tbody></table></div><section class="chart graph-view" data-graph="elevation" hidden><h2>Elevation / metres above sea level</h2><p id="elevationReadout"></p><canvas id="elevationChart" aria-label="Track elevation profile"></canvas></section></div></div></section>
<div class="links"><a href="summary.csv" download>Lap &amp; sector CSV</a><a href="time_gaps.csv" download>Full time-gap CSV</a><a href="session.json" download>Saved car &amp; track settings</a></div>
<p class="note">CSV differences use the initial fastest-car reference. Changing the reference here updates the graphs and table only. Open a car's telemetry report for acceleration and driving/braking forces.</p>
<details><summary>Car setups &amp; conditions</summary><p id="conditions"></p><div class="tablewrap"><table><thead><tr><th>Car</th><th>Mass / kg</th><th>Wheel power / kW</th><th>Cd</th><th>Cl</th><th>Area / m²</th><th>Rear fraction</th><th>Load sensitivity</th></tr></thead><tbody id="setups"></tbody></table></div></details>
<footer><p>Saved locally as a separate session; earlier runs are preserved. All cars use the same track, tire friction, lap mode and calculation spacing. Each qualifying car has its own converged flying-start speed. Positive gap = behind the reference; a falling gap means gaining time.</p><p>Simplified model: inclined road, altitude-dependent air density, load-sensitive tire grip and available wheel power; no gears, load transfer or tire temperature. Map closure is checked for endpoint position and heading. This report works offline. Geometry: <a href="https://github.com/bacinger/f1-circuits">bacinger/f1-circuits</a> (MIT). Elevation: <a href="https://www.opentopodata.org/datasets/srtm/">SRTM90m / Open Topo Data</a>.</p></footer>
</main><script>
'use strict';
const session={
)HTML";
    out << "name:" << jsonString(path.parent_path().filename().string()) << ",mode:"
        << jsonString(mode == LapMode::Qualifying ? "Qualifying lap" : "Standing start")
        << ",mu:" << mu << ",length:" << entries.front().result.totalDistance
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
const colors=['#64e3bc','#efce7c','#85b9ff','#ff968a','#d5a0ff','#ed91c5'];
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let reference=session.reference,cursor=0,curves=[],inspectionTime=0;const shown=session.cars.map(()=>true);
const label=c=>`${c.name} #${c.id}`, dot=i=>`<i class="dot" style="background:${colors[i]}"></i>`;
const order=session.cars.map((_,i)=>i).sort((a,b)=>session.cars[a].lap-session.cars[b].lap);
const winner=session.cars[order[0]];
$('heading').textContent=session.cars.length>1?'Where the time goes.':'Your lap, recorded.';
$('subtitle').textContent=`${session.cars.length} car${session.cars.length>1?'s':''} · ${session.name}`;
$('heading').textContent=trackName+' / car comparison';$('mode').textContent=session.mode;$('winnerLabel').textContent=session.cars.length>1?'FASTEST LAP':'LAP TIME';$('winnerTime').textContent=formatLapTime(winner.lap);$('winnerName').textContent=label(winner);$('length').textContent=f(session.length,1)+' m';
$('startMode').textContent=session.mode==='Qualifying lap'?'Flying lap':'0 km/h';$('startNote').textContent=session.mode==='Qualifying lap'?'Each car matches its start and finish speed':'Braking still accounts for the following lap';
$('reference').innerHTML=session.cars.map((c,i)=>`<option value="${i}">${esc(label(c))}</option>`).join('');$('reference').value=reference;
$('legend').innerHTML=session.cars.map((c,i)=>`<label><input type="checkbox" data-car="${i}" checked>${dot(i)}${esc(label(c))}</label>`).join('');
$('conditions').textContent=`Tire friction coefficient: ${session.mu}. Same track and mode for every car. Rear-wheel drive; static axle loads; tire load sensitivity enabled. Power is specified at the wheels.`;
$('setups').innerHTML=session.cars.map((c,i)=>`<tr><td>${dot(i)}${esc(label(c))}</td>${c.setup.map(v=>`<td>${f(v,2)}</td>`).join('')}</tr>`).join('');
function refreshReference(){const r=session.cars[reference];$('gapNote').textContent=`Relative to ${label(r)}. Above zero = behind; below zero = ahead. A falling line means gaining time.`;
 $('results').innerHTML=order.map(i=>{const c=session.cars[i],values=[c.lap,...c.sectors],ref=[r.lap,...r.sectors];let top=0;for(const p of c.trace)top=Math.max(top,p[2]*3.6);return `<tr><td>${dot(i)}${esc(label(c))}${i===reference?'<small>REFERENCE</small>':''}</td>${values.map((v,j)=>`<td class="${Math.abs(v-Math.min(...session.cars.map(c=>j===0?c.lap:c.sectors[j-1])))<1e-9?'best':''}">${j===0?formatLapTime(v):f(v)+' s'}<small>${delta(v-ref[j])}</small></td>`).join('')}<td>${f(top,1)} km/h</td><td><a href="${encodeURIComponent(c.folder)}/lap_report.html">Open report</a></td></tr>`;}).join('');
 curves=session.cars.map(c=>thin(c.trace.map(p=>({x:p[0],v:p[2]*3.6,g:p[1]-sampleAt(r.trace,p[0]).time}))));update(distanceAtTime(inspectionTime),inspectionTime);
}
const charts=[{id:'speedChart',key:'v'},{id:'gapChart',key:'g'}];
function draw(chart){const canvas=$(chart.id);if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight,dpr=window.devicePixelRatio||1;const ctx=prepareCanvas(canvas);const left=57,right=w-16,top=29,bottom=h-32;chart.left=left;chart.right=right;let low=0,high=0;curves.forEach((ps,i)=>{if(shown[i])for(const p of ps){low=Math.min(low,p[chart.key]);high=Math.max(high,p[chart.key]);}});const pad=Math.max(chart.key==='v'?1:.02,(high-low)*.12);high+=pad;if(low<0)low-=pad;const px=x=>left+x/session.length*(right-left),py=y=>bottom-(y-low)/(high-low)*(bottom-top);ctx.font='11px system-ui';ctx.lineWidth=1;
 for(let i=0;i<3;i++){ctx.fillStyle=i%2?'#ffffff03':'#ffffff07';ctx.fillRect(px(bounds[i]),top,px(bounds[i+1])-px(bounds[i]),bottom-top);ctx.textAlign='center';ctx.fillStyle='#adbec3';ctx.fillText(`SECTOR ${i+1}`,px((bounds[i]+bounds[i+1])/2),16);}
 for(let i=0;i<=4;i++){const v=low+(high-low)*i/4;ctx.strokeStyle='#314044';ctx.beginPath();ctx.moveTo(left,py(v));ctx.lineTo(right,py(v));ctx.stroke();ctx.textAlign='right';ctx.fillStyle='#adbec3';ctx.fillText(f(v,chart.key==='v'?0:2),left-9,py(v)+4);}
 for(let i=0;i<=3;i++){let x=px(bounds[i]);ctx.setLineDash([4,5]);ctx.strokeStyle='#536268';ctx.beginPath();ctx.moveTo(x,top);ctx.lineTo(x,bottom);ctx.stroke();ctx.setLineDash([]);ctx.textAlign=i===0?'left':i===3?'right':'center';ctx.fillText(f(bounds[i],0)+' m',x,h-10);}
 if(chart.key==='g'){ctx.strokeStyle='#afc2c7';ctx.beginPath();ctx.moveTo(left,py(0));ctx.lineTo(right,py(0));ctx.stroke();}
 curves.forEach((ps,i)=>{if(!shown[i])return;ctx.strokeStyle=colors[i];ctx.lineWidth=i===reference?1.5:2;ctx.setLineDash(chart.key==='g'&&i===reference?[5,4]:[]);ctx.beginPath();ps.forEach((p,j)=>{if(j)ctx.lineTo(px(p.x),py(p[chart.key]));else ctx.moveTo(px(p.x),py(p[chart.key]));});ctx.stroke();});ctx.setLineDash([]);
 ctx.strokeStyle='#edf6f3';ctx.lineWidth=1;ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(px(cursor),top);ctx.lineTo(px(cursor),bottom);ctx.stroke();ctx.setLineDash([]);atPosition(session.cars,reference,cursor).forEach((p,i)=>{if(!shown[i])return;ctx.fillStyle=colors[i];ctx.beginPath();ctx.arc(px(cursor),py(chart.key==='v'?p.speed*3.6:p.gap),3,0,Math.PI*2);ctx.fill();});
 if(!shown.some(Boolean)){ctx.fillStyle='#adbec3';ctx.textAlign='center';ctx.fillText('Select a car above to display its trace.',(left+right)/2,(top+bottom)/2);}
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
function installMapZoom(){$('followCar').innerHTML='<option value="none">Off</option>'+session.cars.map((car,i)=>'<option value="'+i+'">'+esc(label(car))+'</option>').join('');$('followCar').addEventListener('change',()=>redrawMapView());const canvas=$('trackMap');canvas.style.touchAction='none';$('zoomIn').addEventListener('click',()=>zoomMap(mapZoom*1.3));$('zoomOut').addEventListener('click',()=>zoomMap(mapZoom/1.3));$('zoomReset').addEventListener('click',()=>{mapZoom=1;mapPanX=mapPanY=0;$('followCar').value='none';redrawMapView();});canvas.addEventListener('wheel',e=>{e.preventDefault();const rect=canvas.getBoundingClientRect();zoomMap(mapZoom*Math.exp(-e.deltaY*.0015),e.clientX-rect.left-canvas.clientWidth/2,e.clientY-rect.top-canvas.clientHeight/2);},{passive:false});canvas.addEventListener('pointerdown',e=>{$('followCar').value='none';mapDrag={id:e.pointerId,x:e.clientX,y:e.clientY};canvas.setPointerCapture(e.pointerId);});canvas.addEventListener('pointermove',e=>{if(!mapDrag||e.pointerId!==mapDrag.id)return;mapPanX+=e.clientX-mapDrag.x;mapPanY+=e.clientY-mapDrag.y;mapDrag.x=e.clientX;mapDrag.y=e.clientY;redrawMapView();});const stop=e=>{if(mapDrag&&e.pointerId===mapDrag.id)mapDrag=null;};canvas.addEventListener('pointerup',stop);canvas.addEventListener('pointercancel',stop);canvas.addEventListener('lostpointercapture',stop);}
function installPlayback(){installMapZoom();document.querySelectorAll('[data-view]').forEach(button=>button.addEventListener('click',()=>{document.querySelectorAll('[data-view]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));document.querySelectorAll('[data-graph]').forEach(panel=>panel.hidden=panel.dataset.graph!==button.dataset.view);renderVersion++;update(cursor,inspectionTime);}));$('play').addEventListener('click',()=>setPlaying(!playing));$('orientation').addEventListener('change',e=>{mapRotation=e.target.value==='typical'?defaultMapRotation:e.target.value==='north'?0:defaultMapRotation+Number(e.target.value);renderVersion++;update(cursor,inspectionTime);});$('scrub').addEventListener('pointerdown',()=>setPlaying(false));$('scrub').addEventListener('keydown',()=>setPlaying(false));$('reset').addEventListener('click',()=>{setPlaying(false);playbackTime=0;update(0);});}

function sectorAtDistance(x){return x<bounds[1]?1:x<bounds[2]?2:3;}
function mapAt(d){let lo=0,hi=trackMap.length-1;while(lo<hi){let m=(lo+hi)>>1;if(trackMap[m][0]<d)lo=m+1;else hi=m;}if(!lo)return trackMap[0];const a=trackMap[lo-1],b=trackMap[lo],t=Math.max(0,Math.min(1,(d-a[0])/(b[0]-a[0])));return [d,a[1]+t*(b[1]-a[1]),a[2]+t*(b[2]-a[2])];}
function drawElevation(d){const canvas=$('elevationChart');if(!canvas.clientWidth)return;const w=canvas.clientWidth,h=canvas.clientHeight;let low=Infinity,high=-Infinity;for(const p of elevationProfile){low=Math.min(low,p[1]);high=Math.max(high,p[1]);}const pad=Math.max(2,(high-low)*.15);low-=pad;high+=pad;const px=x=>50+x/bounds[3]*(w-70),py=y=>h-30-(y-low)/(high-low)*(h-55);
const ctx=cachedBackground(canvas,'elevation',bg=>{bg.font='11px system-ui';bg.fillStyle='#adbec3';for(let i=0;i<5;i++){const y=low+(high-low)*i/4;bg.fillText(y.toFixed(0)+' m',2,py(y));bg.strokeStyle='#314044';bg.beginPath();bg.moveTo(50,py(y));bg.lineTo(w-20,py(y));bg.stroke();}bg.strokeStyle='#efce7c';bg.lineWidth=2;bg.beginPath();elevationProfile.forEach((p,i)=>i?bg.lineTo(px(p[0]),py(p[1])):bg.moveTo(px(p[0]),py(p[1])));bg.stroke();});ctx.strokeStyle='#edf6f3';ctx.beginPath();ctx.moveTo(px(d),20);ctx.lineTo(px(d),h-30);ctx.stroke();let lo=0,hi=elevationProfile.length-1;while(lo<hi){const m=(lo+hi)>>1;if(elevationProfile[m][0]<d)lo=m+1;else hi=m;}const p=elevationProfile[lo];$('elevationReadout').textContent=p[1].toFixed(1)+' m altitude · '+(p[2]*100).toFixed(1)+'% gradient · '+(elevationEnabled?'Elevation physics enabled':'Elevation physics disabled')+'. Terrain estimate; not a surveyed road profile.';}
function drawMap(d){const canvas=$('trackMap'),w=canvas.clientWidth,h=canvas.clientHeight;if(!rotatedGeometry||rotatedGeometry.rotation!==mapRotation){const points=trackMap.map(rotatePoint);let xmin=Infinity,xmax=-Infinity,ymin=Infinity,ymax=-Infinity;for(const p of points){xmin=Math.min(xmin,p[1]);xmax=Math.max(xmax,p[1]);ymin=Math.min(ymin,p[2]);ymax=Math.max(ymax,p[2]);}rotatedGeometry={rotation:mapRotation,points,xmin,xmax,ymin,ymax};}const {points:rotated,xmin,xmax,ymin,ymax}=rotatedGeometry;const scale=mapZoom*Math.min((w-80)/Math.max(1,xmax-xmin),(h-70)/Math.max(1,ymax-ymin)),px=x=>w/2+mapPanX+(x-(xmin+xmax)/2)*scale,py=y=>h/2+mapPanY-(y-(ymin+ymax)/2)*scale;
if($('followCar').value!=='none'){const target=rotatePoint(mapAt(carDistanceAtTime(session.cars[Number($('followCar').value)],inspectionTime)));mapPanX=-(target[1]-(xmin+xmax)/2)*scale;mapPanY=(target[2]-(ymin+ymax)/2)*scale;}
const ctx=cachedBackground(canvas,'map'+renderVersion+':'+mapPanX+':'+mapPanY,bg=>{const colors=['#64e3bc','#efce7c','#ff887b'];bg.lineWidth=5;bg.lineCap='round';for(let s=0;s<3;s++){const ps=[rotatePoint(mapAt(bounds[s])),...rotated.filter(p=>p[0]>bounds[s]&&p[0]<bounds[s+1]),rotatePoint(mapAt(bounds[s+1]))];bg.strokeStyle=colors[s];bg.beginPath();ps.forEach((p,i)=>i?bg.lineTo(px(p[1]),py(p[2])):bg.moveTo(px(p[1]),py(p[2])));bg.stroke();}bg.font='11px system-ui';bg.fillStyle='#edf6f3';turnLabels.map(rotatePoint).forEach((p,i)=>bg.fillText('T'+(i+1),px(p[1])+(i%2?-23:7),py(p[2])+(i%2?12:-9)));for(let i=0;i<4;i++){const p=rotatePoint(mapAt(bounds[i]));bg.fillRect(px(p[1])-3,py(p[2])-3,6,6);bg.fillText(i===0?'Start':i===3?'Finish':'S'+i,px(p[1])+8,py(p[2])+14+(i===3&&mapClosed?12:0));}});
const markers=carMarkersAtTime(inspectionTime);markers.forEach(marker=>{const position=marker.distance,p=rotatePoint(mapAt(position)),ahead=rotatePoint(mapAt(Math.min(bounds[3],position+3))),behind=rotatePoint(mapAt(Math.max(0,position-3)));const heading=Math.atan2(py(ahead[2])-py(behind[2]),px(ahead[1])-px(behind[1]));const collisions=markers.filter(other=>Math.abs(other.distance-position)<12);const lane=collisions.length>1?(collisions.indexOf(marker)-(collisions.length-1)/2)*7:0;const x=px(p[1])-Math.sin(heading)*lane,y=py(p[2])+Math.cos(heading)*lane;ctx.save();ctx.translate(x,y);ctx.rotate(heading);ctx.fillStyle=colors[marker.index];ctx.strokeStyle='#101719';ctx.lineWidth=1.5;ctx.beginPath();ctx.moveTo(9,0);ctx.lineTo(-6,-5);ctx.lineTo(-3,0);ctx.lineTo(-6,5);ctx.closePath();ctx.fill();ctx.stroke();ctx.restore();ctx.font='bold 11px system-ui';ctx.fillStyle=colors[marker.index];ctx.fillText('#'+(marker.index+1),x+10,y-10-marker.index*10);});$('mapNote').textContent='S1 green · S2 gold · S3 coral · '+(mapClosed?'Closed circuit':'Open route; endpoint gap '+closureGap.toFixed(1)+' m')+'. Colored arrows show each visible car at the same elapsed time. Finished cars wait until the slowest car finishes. '+dataNote;drawElevation(d);}

function elapsedAt(d){return sampleAt(session.cars[reference].trace,d).time;}function lapDuration(){return Math.max(...session.cars.map(car=>car.lap));}
function carDistanceAtTime(car,t){const trace=car.trace;if(t<=0)return 0;if(t>=car.lap)return session.length;let lo=0,hi=trace.length-1;while(lo+1<hi){const m=(lo+hi)>>1;if(trace[m][1]<=t)lo=m;else hi=m;}const a=trace[lo],b=trace[hi],acc=(b[2]*b[2]-a[2]*a[2])/(2*(b[0]-a[0])),dt=Math.max(0,t-a[1]);return Math.min(b[0],a[0]+a[2]*dt+.5*acc*dt*dt);}
function distanceAtTime(t){return carDistanceAtTime(session.cars[reference],t);}
function carMarkersAtTime(t){return session.cars.map((car,index)=>({index,distance:carDistanceAtTime(car,t)})).filter(marker=>shown[marker.index]);}

function update(x,time=null){cursor=Math.max(0,Math.min(session.length,x));inspectionTime=time===null?elapsedAt(cursor):time;$('scrub').value=cursor/session.length*10000;$('position').textContent=`${f(cursor,1)} m · Sector ${sectorAtDistance(cursor)}`;const samples=atPosition(session.cars,reference,cursor);$('inspection').innerHTML=session.cars.map((c,i)=>`<tr style="opacity:${shown[i]?1:.45}"><td>${dot(i)}${esc(label(c))}</td><td>${f(samples[i].speed*3.6,1)} km/h</td><td>${f(samples[i].time)} s</td><td>${delta(samples[i].gap)}</td></tr>`).join('');charts.forEach(draw);drawMap(cursor);}
$('reference').addEventListener('change',e=>{reference=Number(e.target.value);renderVersion++;refreshReference();if(playing){playbackTime=inspectionTime;lastFrame=null;}});$('legend').addEventListener('change',e=>{const i=Number(e.target.dataset.car);if(Number.isInteger(i)&&i>=0&&i<shown.length){shown[i]=e.target.checked;renderVersion++;update(cursor,inspectionTime);}});$('scrub').addEventListener('input',e=>{setPlaying(false);update(Number(e.target.value)/10000*session.length);});$('reset').addEventListener('click',()=>update(0));
for(const chart of charts)for(const event of ['pointermove','pointerdown'])$(chart.id).addEventListener(event,e=>{if(event==='pointermove'&&playing)return;if(event==='pointerdown')setPlaying(false);const rect=e.currentTarget.getBoundingClientRect();update((e.clientX-rect.left-chart.left)/(chart.right-chart.left)*session.length);});window.addEventListener('resize',()=>update(cursor,inspectionTime));installPlayback();refreshReference();
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
    std::cout << "\nF1 LAP TIME SIMULATOR\nEnter accepts the preset default. B goes back one answer; Q quits.\n";
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
    std::cout << "1. Qualifying lap (flying start)\n2. Standing start (0 m/s)\n";
    if (!readInteger("Select a lap mode: ", lapChoice, 1, 2)) return 1;
    LapMode mode = lapChoice == 1 ? LapMode::Qualifying : LapMode::StandingStart;
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
            entry.result = simulateTrack(entry.car, track, frictionCoefficient,
                                          airDensity, 0.0, distanceStep, mode, elevationEnabled);
            entry.sectors = sectorTimes(entry.result);
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
    if (entries.size() == 1) printSolo(entries.front(), track, airDensity);
    const std::size_t reference = fastestEntry(entries);
    printComparison(entries, reference);

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
                    int lap=static_cast<int>(conditions.number("Lap mode (1 qualifying, 2 standing start)",mode==LapMode::Qualifying ? 1 : 2,1,2,true));
                    double grip=conditions.number("Tire grip mu",frictionCoefficient,0.000001,100);
                    int elevation=static_cast<int>(conditions.number("Elevation (1 enabled, 2 disabled)",elevationEnabled ? 1 : 2,1,2,true));
                    mode=lap==1 ? LapMode::Qualifying : LapMode::StandingStart;
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
