// pulse_source.hpp - where pulses come from: the kernel driver or a simulator
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "dataset.hpp"

struct PulseEvent {
    uint64_t seq;
    uint64_t ts_ns;  // CLOCK_MONOTONIC (device) or virtual clock (simulator)
};

class PulseSource {
public:
    virtual ~PulseSource() = default;
    // returns 1 = event stored, 0 = timeout, -1 = error
    virtual int wait_event(PulseEvent& ev, int timeout_ms) = 0;
    virtual uint64_t now_ns() const = 0;       // clock matching the event timestamps
    virtual uint32_t pulses_per_kwh() const = 0;
    virtual std::string name() const = 0;
    virtual std::time_t wall_start() const { return std::time(nullptr); }  // wall time of first pulse
    virtual bool finished() const { return false; }                        // true when a finite source is exhausted
};

// Reads /dev/smeter0 (character device driver) using poll(2) + read(2). RAII fd.
class DeviceSource : public PulseSource {
public:
    explicit DeviceSource(const std::string& path);  // throws std::runtime_error
    ~DeviceSource() override;
    DeviceSource(const DeviceSource&) = delete;
    DeviceSource& operator=(const DeviceSource&) = delete;

    int wait_event(PulseEvent& ev, int timeout_ms) override;
    uint64_t now_ns() const override;
    uint32_t pulses_per_kwh() const override { return ppk_; }
    std::string name() const override { return "device " + path_; }

private:
    std::string path_;
    int fd_ = -1;
    uint32_t ppk_ = 1000;
};

// In-process load simulator so the analytics can be demoed without root/driver.
// Virtual time runs `speed` times faster than the wall clock.
class SimSource : public PulseSource {
public:
    SimSource(uint32_t ppk, double speed, bool with_outage);

    int wait_event(PulseEvent& ev, int timeout_ms) override;
    uint64_t now_ns() const override;
    uint32_t pulses_per_kwh() const override { return ppk_; }
    std::string name() const override { return "simulator"; }

private:
    double load_at(double vt_s);
    uint32_t ppk_;
    double speed_;
    bool outage_;
    uint64_t seq_ = 0;
    double next_vt_s_ = 1.0;
    std::chrono::steady_clock::time_point t0_;
    std::mt19937 rng_{42};
};

// Replays a recorded consumption dataset (watts per interval) as the pulse train a real
// meter with `ppk` imp/kWh would have produced. Virtual time runs `speed` x faster.
class ReplaySource : public PulseSource {
public:
    ReplaySource(const std::vector<Sample>& samples, uint32_t ppk, double speed);

    int wait_event(PulseEvent& ev, int timeout_ms) override;
    uint64_t now_ns() const override;
    uint32_t pulses_per_kwh() const override { return ppk_; }
    std::string name() const override { return "dataset replay"; }
    std::time_t wall_start() const override { return wall_start_; }
    bool finished() const override { return idx_ >= ts_.size(); }
    std::size_t pulse_count() const { return ts_.size(); }

private:
    uint32_t ppk_;
    double speed_;
    std::time_t wall_start_ = 0;
    std::vector<uint64_t> ts_;     // pre-computed pulse timestamps (virtual ns)
    std::size_t idx_ = 0;
    std::chrono::steady_clock::time_point t0_;
};
