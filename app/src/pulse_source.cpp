#include "pulse_source.hpp"
#include "smeter_ioctl.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <thread>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

// ---------------------------------------------------------------- DeviceSource
DeviceSource::DeviceSource(const std::string& path) : path_(path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd_ < 0)
        throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno) +
                                 " (is the driver loaded? try: make load)");
    smeter_stats st{};
    if (::ioctl(fd_, SMETER_IOC_GET_STATS, &st) == 0 && st.pulses_per_kwh) ppk_ = st.pulses_per_kwh;
}

DeviceSource::~DeviceSource() {
    if (fd_ >= 0) ::close(fd_);
}

int DeviceSource::wait_event(PulseEvent& ev, int timeout_ms) {
    pollfd pfd{fd_, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    smeter_event raw{};
    ssize_t n = ::read(fd_, &raw, sizeof raw);
    if (n == static_cast<ssize_t>(sizeof raw)) {
        ev = {raw.seq, raw.ts_ns};
        return 1;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;
}

uint64_t DeviceSource::now_ns() const { return monotonic_ns(); }

// ------------------------------------------------------------------ SimSource
SimSource::SimSource(uint32_t ppk, double speed, bool with_outage)
    : ppk_(ppk ? ppk : 1000), speed_(speed > 0 ? speed : 1.0), outage_(with_outage),
      t0_(std::chrono::steady_clock::now()) {}

uint64_t SimSource::now_ns() const {
    double real_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    return static_cast<uint64_t>(real_s * speed_ * 1e9);
}

// Household-like profile: slow base load + noise, heater burst for 5 s every minute.
double SimSource::load_at(double vt_s) {
    std::normal_distribution<double> noise(0.0, 25.0);
    double load = 1200.0 + 400.0 * std::sin(vt_s / 30.0) + noise(rng_);
    if (std::fmod(vt_s, 60.0) >= 45.0 && vt_s > 60.0) load = 6000.0;
    return load < 100.0 ? 100.0 : load;
}

int SimSource::wait_event(PulseEvent& ev, int timeout_ms) {
    const double deadline_vt = now_ns() / 1e9 + timeout_ms / 1000.0 * speed_;
    if (next_vt_s_ > deadline_vt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        if (now_ns() / 1e9 < next_vt_s_) return 0;
    } else {
        double wait_real = (next_vt_s_ - now_ns() / 1e9) / speed_;
        if (wait_real > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait_real));
    }
    ev = {++seq_, static_cast<uint64_t>(next_vt_s_ * 1e9)};
    const double period = 3.6e6 / (ppk_ * load_at(next_vt_s_));  // seconds per pulse
    next_vt_s_ += period;
    if (outage_ && next_vt_s_ > 120.0 && next_vt_s_ < 170.0) next_vt_s_ = 170.0;  // 50 s blackout
    return 1;
}

// ---------------------------------------------------------------- ReplaySource
// Energy -> pulses: inside each interval the power is constant, so the next pulse
// happens after (energy still missing for one pulse) / power. `carry` keeps the
// fraction of a pulse between intervals, so total energy is conserved exactly.
ReplaySource::ReplaySource(const std::vector<Sample>& s, uint32_t ppk, double speed)
    : ppk_(ppk ? ppk : 1000), speed_(speed > 0 ? speed : 1.0), t0_(std::chrono::steady_clock::now()) {
    const double wh = 1000.0 / ppk_;
    double carry = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        double dt = (i + 1 < s.size()) ? double(s[i + 1].t - s[i].t)
                                       : (i > 0 ? double(s[i].t - s[i - 1].t) : 60.0);
        const double p = s[i].watts;
        double pos = double(s[i].t - s[0].t), rem = dt;
        if (p <= 0) continue;
        for (;;) {
            const double tt = (wh - carry) * 3600.0 / p;   // seconds until next pulse
            if (tt <= rem) {
                pos += tt; rem -= tt; carry = 0.0;
                ts_.push_back(static_cast<uint64_t>(pos * 1e9));
            } else {
                carry += p * rem / 3600.0;
                break;
            }
        }
    }
    wall_start_ = s.empty() ? std::time(nullptr)
                            : s[0].t + static_cast<std::time_t>(ts_.empty() ? 0 : ts_[0] / 1000000000ULL);
}

uint64_t ReplaySource::now_ns() const {
    double real_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    return static_cast<uint64_t>(real_s * speed_ * 1e9);
}

int ReplaySource::wait_event(PulseEvent& ev, int timeout_ms) {
    if (finished()) return 0;
    const uint64_t target = ts_[idx_];
    const uint64_t now = now_ns();
    if (target > now) {
        double wait_real = double(target - now) / 1e9 / speed_;
        if (wait_real > timeout_ms / 1000.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(wait_real));
    }
    ev = {idx_ + 1, target};
    ++idx_;
    return 1;
}
