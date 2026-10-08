// Compile this as a standalone C++17 program, then pass the tracks directory.
#define main simulatorInteractiveMain
#include "../f1_track_sim.cpp"
#undef main

int main(int argc, char* argv[])
{
    if (argc != 2) { std::cerr << "Usage: validate_circuits <tracks directory>\n"; return 2; }
    try {
        const auto circuits = discoverCircuits(argv[1]);
        auto car = presetEntry(2).car;
        for (const auto& circuit : circuits) {
            const auto track = loadTrackFile(circuit.path);
            for (auto mode : {LapMode::Qualifying, LapMode::StandingStart}) {
                const auto result = simulateTrack(car, track, 1.0, 1.225, 0.0, 0.25, mode, true);
                if (!result.closed || !std::isfinite(result.totalTime) || result.totalTime <= 0
                    || std::abs(result.totalDistance - track.profile.back().distance) > 1e-5)
                    throw std::runtime_error("Invalid lap result: " + circuit.name);
                for (double speed : result.speed)
                    if (!std::isfinite(speed) || speed < 0)
                        throw std::runtime_error("Invalid speed: " + circuit.name);
                if (mode == LapMode::StandingStart && result.speed.front() != 0)
                    throw std::runtime_error("Standing start is not stationary: " + circuit.name);
                if (mode == LapMode::Qualifying && std::abs(result.speed.front() - result.speed.back()) > 1e-8)
                    throw std::runtime_error("Flying lap speed is discontinuous: " + circuit.name);
                const auto sectors = sectorTimes(result);
                if (std::abs(sectors[0] + sectors[1] + sectors[2] - result.totalTime) > 1e-6)
                    throw std::runtime_error("Sector sum mismatch: " + circuit.name);
                std::cout << circuit.path.filename().string() << ','
                          << (mode == LapMode::Qualifying ? "qualifying" : "standing")
                          << ',' << result.totalTime << '\n' << std::flush;
            }
        }
        std::cout << "PASS: " << circuits.size() << " circuits, both lap modes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
