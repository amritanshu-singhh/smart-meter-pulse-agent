// analytics.hpp - energy analytics engine (pure logic, no I/O, unit-testable)
#pragma once
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "model.hpp"

enum class MeterState { Idle, Normal, Spike, NoSignal };
const char* to_string(MeterState s);

// Illustrative time-of-use tariff in Rs/kWh.
struct Tariff {
    double offpeak = 5.0, normal = 7.0, peak = 9.0;
    int peak_start = 18, peak_end = 22;       // [18:00, 22:00)
    int offpeak_start = 22, offpeak_end = 6;  // [22:00, 06:00)
    double rate_at(int hour) const;
};

struct AnalyticsConfig {
    uint32_t pulses_per_kwh = 1000;  // meter constant
    double window_s = 60.0;          // sliding window for average demand
    double spike_factor = 2.5;       // inst > factor * baseline  => SPIKE
    double recover_factor = 1.3;     // inst < factor * baseline  => back to NORMAL
    unsigned warmup_intervals = 5;   // intervals needed before spike detection
    double no_signal_s = 30.0;       // silence longer than this => NO_SIGNAL
    double ewma_alpha = 0.2;         // baseline smoothing
    Tariff tariff;
    const HourlyModel* model = nullptr;  // learned baseline (optional); replaces the ratio rule
};

struct Summary {
    uint64_t pulses = 0;
    double energy_kwh = 0, inst_w = 0, avg_w = 0, peak_w = 0, cost = 0;
    double elapsed_s = 0, projected_kwh_month = 0;
    MeterState state = MeterState::Idle;
};

// Thread-safe: on_pulse()/check_timeout() from the analytics thread,
// snapshot()/drain_alerts() from the reporting thread.
class Analytics {
public:
    Analytics(const AnalyticsConfig& cfg, std::time_t wall_start);

    Summary on_pulse(uint64_t ts_ns);          // feed one pulse timestamp
    void check_timeout(uint64_t now_ns);       // detect loss of signal
    Summary snapshot() const;
    std::vector<std::string> drain_alerts();

private:
    Summary make_summary() const;              // mutex must be held
    void alert(uint64_t ts_ns, const std::string& msg);

    AnalyticsConfig cfg_;
    std::time_t wall_start_;
    double wh_per_pulse_;
    mutable std::mutex m_;

    bool have_first_ = false;
    uint64_t first_ts_ = 0, last_ts_ = 0;
    uint64_t pulses_ = 0, intervals_ = 0;
    int hour_ = 0;
    double energy_wh_ = 0, cost_ = 0, inst_w_ = 0, avg_w_ = 0, peak_w_ = 0, ewma_w_ = 0;
    MeterState state_ = MeterState::Idle;
    std::deque<uint64_t> window_;
    std::vector<std::string> alerts_;
};
