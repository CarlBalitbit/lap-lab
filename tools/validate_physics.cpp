// Standalone mathematical verification; expected values never call simulator helpers.
#define main simulatorInteractiveMain
#include "../f1_track_sim.cpp"
#undef main

namespace physicsValidation {
int checks = 0, failures = 0;

void check(const std::string& name, double actual, double expected,
           double absolute, double relative)
{
    ++checks;
    const double error = std::abs(actual - expected);
    const double tolerance = absolute + relative * std::abs(expected);
    const bool passed = std::isfinite(actual) && std::isfinite(expected) && error <= tolerance;
    if (!passed) ++failures;
    std::cout << (passed ? "PASS," : "FAIL,") << name << ",actual=" << actual
              << ",expected=" << expected << ",error=" << error << ",limit=" << tolerance << '\n';
}

// Independent reference: resolve gravity through an angle, then sum two tires.
double capacity(const Car& car, double mu, double rho, double v, double q, double grade)
{
    const double normal = car.mass * 9.81 * std::cos(std::atan(grade))
                        + rho * car.liftCoefficient * car.frontalArea * v * v / 2;
    const double tireLoad = q * normal / 2;
    return 2 * mu * tireLoad * std::pow(car.referenceTireLoad / tireLoad, car.loadSensitivity);
}

void forces()
{
    Car car;
    car.mass = 800; car.power = 80; car.frontalArea = 1.5;
    car.dragCoefficient = 0.8; car.liftCoefficient = 2.3;
    for (double rho : {1.0, 1.225}) for (double v : {0.0, 20.0, 60.0}) {
        const auto label = "rho=" + std::to_string(rho) + ",v=" + std::to_string(v);
        check("drag," + label, car.dragForce(rho,v), rho*0.8*1.5*v*v/2, 1e-8,1e-10);
        check("downforce," + label, car.downforce(rho,v), rho*2.3*1.5*v*v/2, 1e-8,1e-10);
        for (double grade : {-0.1,0.0,0.1}) for (double q : {0.45,0.55})
            check("axle capacity," + label + ",grade=" + std::to_string(grade) + ",q=" + std::to_string(q),
                  car.axleCapacity(1,rho,v,q,grade),capacity(car,1,rho,v,q,grade),1e-8,1e-10);
    }
    const double v=20, curvature=0.01, rho=1.225;
    double total=0;
    for (double q : {0.45,0.55}) {
        const double cap=capacity(car,1,rho,v,q,0), lateral=q*800*v*v*curvature;
        const double available=std::sqrt(cap*cap-lateral*lateral);
        check("combined grip,q="+std::to_string(q),car.axleGrip(1,rho,v,curvature,q),available,1e-8,1e-10);
        total+=available;
        if(q==0.55) check("rear-only drive grip",car.driveGrip(1,rho,v,curvature),available,1e-8,1e-10);
    }
    check("both-axle longitudinal grip",car.longitudinalGrip(1,rho,v,curvature),total,1e-8,1e-10);
    car.liftCoefficient=0;
    for(double radius : {25.0,60.0,150.0}) {
        double expected=std::numeric_limits<double>::infinity();
        for(double q : {0.45,0.55}) expected=std::min(expected,std::sqrt(capacity(car,1,rho,0,q,0)*radius/(q*800)));
        check("corner radius="+std::to_string(radius),car.corneringSpeed(1,radius,rho),expected,1e-8,1e-9);
    }
    // Equal endpoints isolate force resolution from the endpoint-bound approximation.
    for(double grade : {-0.1,0.0,0.1}) {
        Cell cell{1,0,0,grade,0,rho};
        const double rear=capacity(car,1,rho,v,0.55,grade);
        const double front=capacity(car,1,rho,v,0.45,grade);
        const double drag=rho*0.8*1.5*v*v/2;
        const double gravity=9.81*std::sin(std::atan(grade));
        const double drive=(std::min(rear,80000/v)-drag)/800-gravity;
        const double brake=(front+rear+drag)/800+gravity;
        check("incline drive,grade="+std::to_string(grade),driveBound(car,1,rho,cell,v,v),drive,1e-9,1e-10);
        check("incline brake,grade="+std::to_string(grade),brakeBound(car,1,rho,cell,v,v),brake,1e-9,1e-10);
    }
}

void timing()
{
    for (const auto& fixture : std::vector<std::array<double,3>>{{10,2,20},{20,-2,20},{10,0,20},{0,2,20}}) {
        const double v=fixture[0], a=fixture[1], distance=fixture[2];
        const double end=std::sqrt(v*v+2*a*distance);
        const double expected=a==0 ? distance/v : (end-v)/a;
        check("partialTime,v="+std::to_string(v)+",a="+std::to_string(a),partialTime(v,a,distance),expected,1e-9,1e-10);
    }
    // v(t)=10+2t, s(t)=10t+t^2; two 12 m cells, sectors inside cells.
    Simulation fixture;
    fixture.cells={{12,0,0},{12,0,0}};
    fixture.speed={10,std::sqrt(148.0),14};
    fixture.sectorBounds={{0,5,17,24}};
    auto timeAt=[](double s){return (-10+std::sqrt(100+4*s))/2;};
    const auto sectors=sectorTimes(fixture);
    double sum=0;
    for(int i=0;i<3;++i){
        check("synthetic sector="+std::to_string(i+1),sectors[i],timeAt(fixture.sectorBounds[i+1])-timeAt(fixture.sectorBounds[i]),1e-9,1e-10);
        sum+=sectors[i];
    }
    check("synthetic sector sum",sum,2,1e-6,1e-10);
    const auto trace=makeTrace(fixture);
    for(double s : {0.0,5.0,12.0,17.0,24.0}) {
        const auto sample=sampleTrace(trace,s);
        check("trace time,s="+std::to_string(s),sample.time,timeAt(s),1e-9,1e-10);
        check("trace speed,s="+std::to_string(s),sample.speed,std::sqrt(100+4*s),1e-9,1e-10);
    }
}
}

int main()
{
    const auto start=std::chrono::steady_clock::now();
    std::cout << std::setprecision(15);
    try { physicsValidation::forces(); physicsValidation::timing(); }
    catch(const std::exception& error){++physicsValidation::failures;std::cerr<<"FAIL,exception,"<<error.what()<<'\n';}
    std::cout << "SUMMARY,checks=" << physicsValidation::checks << ",failures=" << physicsValidation::failures
              << ",runtime_s=" << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << '\n';
    return physicsValidation::failures ? 1 : 0;
}
