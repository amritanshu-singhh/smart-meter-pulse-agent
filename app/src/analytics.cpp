#include "analytics.hpp"
#include <cstdio>

const char* to_string(MeterState s) {
    switch (s) {
        case MeterState::Idle:     return "IDLE";
        case MeterState::Normal:   return "NORMAL";
        case MeterState::Spike:    return "SPIKE";
        case MeterState::NoSignal: return "NO_SIGNAL";
    }
    return "?";
}

double Tariff::rate_at(int hour) const {
    if (hour >= peak_start && hour < peak_end) return peak;
    if (hour >= offpeak_start || hour < offpeak_end) return offpeak;
    return normal;
}

Analytics::Analytics(const AnalyticsConfig& cfg, std::time_t wall_start)
    : cfg_(cfg), wall_start_(wall_start),
      wh_per_pulse_(1000.0 / (cfg.pulses_per_kwh ? cfg.pulses_per_kwh : 1)) {}

void Analytics::alert(uint64_t ts_ns, const std::string& msg) {
    char buf[64];
    double t = have_first_ ? (ts_ns - first_ts_) / 1e9 : 0.0;
    std::snprintf(buf, sizeof buf, "[t=%.1fs] ", t);
    alerts_.push_back(buf + msg);
}

Summary Analytics::on_pulse(uint64_t ts_ns) {
    std::lock_guard<std::mutex> lk(m_);

    // energy + time-of-use cost of this pulse
    std::time_t wall = wall_start_ + (have_first_ ? (ts_ns - first_ts_) / 1000000000ULL : 0);
    std::tm tmv{};
    localtime_r(&wall, &tmv);
    hour_ = tmv.tm_hour;
    ++pulses_;
    energy_wh_ += wh_per_pulse_;
    cost_ += wh_per_pulse_ / 1000.0 * cfg_.tariff.rate_at(tmv.tm_hour);

    if (!have_first_) {
        have_first_ = true;
        first_ts_ = last_ts_ = ts_ns;
        state_ = MeterState::Normal;
        window_.push_back(ts_ns);
        return make_summary();
    }

    const double dt = (ts_ns > last_ts_) ? (ts_ns - last_ts_) / 1e9 : 0.0;
    const bool after_gap = (state_ == MeterState::NoSignal);
    last_ts_ = ts_ns;

    if (after_gap) {                       // first pulse after silence
        state_ = MeterState::Normal;
        alert(ts_ns, "signal restored");
        window_.clear();
        window_.push_back(ts_ns);
        return make_summary();
    }

    if (dt > 0) {
        inst_w_ = wh_per_pulse_ * 3600.0 / dt;
        ++intervals_;
        const bool warmed = intervals_ > cfg_.warmup_intervals;
        char msg[128];
        const bool use_model = cfg_.model && cfg_.model->valid;
        const double z = use_model ? cfg_.model->zscore(hour_, inst_w_) : 0.0;
        if (use_model) {
            if (warmed && state_ != MeterState::Spike && z > cfg_.model->z_threshold) {
                state_ = MeterState::Spike;
                std::snprintf(msg, sizeof msg, "ANOMALY: %.0f W, z=%.1f vs learned %.0f+-%.0f W for hour %02d",
                              inst_w_, z, cfg_.model->mean[hour_], cfg_.model->stdv[hour_], hour_);
                alert(ts_ns, msg);
            } else if (state_ == MeterState::Spike && z < cfg_.model->z_threshold * 0.5) {
                state_ = MeterState::Normal;
                std::snprintf(msg, sizeof msg, "back to normal: %.0f W (z=%.1f)", inst_w_, z);
                alert(ts_ns, msg);
            }
        } else if (warmed && state_ != MeterState::Spike && inst_w_ > cfg_.spike_factor * ewma_w_) {
            state_ = MeterState::Spike;
            std::snprintf(msg, sizeof msg, "SPIKE: %.0f W vs baseline %.0f W", inst_w_, ewma_w_);
            alert(ts_ns, msg);
        } else if (state_ == MeterState::Spike && inst_w_ < cfg_.recover_factor * ewma_w_) {
            state_ = MeterState::Normal;
            std::snprintf(msg, sizeof msg, "load back to normal: %.0f W", inst_w_);
            alert(ts_ns, msg);
        }
        if (state_ != MeterState::Spike)   // keep the baseline clean of spikes
            ewma_w_ = (intervals_ == 1) ? inst_w_
                      : cfg_.ewma_alpha * inst_w_ + (1 - cfg_.ewma_alpha) * ewma_w_;
    }

    // sliding window average demand
    window_.push_back(ts_ns);
    const uint64_t win_ns = static_cast<uint64_t>(cfg_.window_s * 1e9);
    while (window_.size() > 2 && ts_ns - window_.front() > win_ns) window_.pop_front();
    if (window_.size() >= 2) {
        const double span = (window_.back() - window_.front()) / 1e9;
        if (span > 0) avg_w_ = (window_.size() - 1) * wh_per_pulse_ * 3600.0 / span;
        if (window_.size() >= 3 && avg_w_ > peak_w_) peak_w_ = avg_w_;
    }
    return make_summary();
}

void Analytics::check_timeout(uint64_t now_ns) {
    std::lock_guard<std::mutex> lk(m_);
    if (!have_first_ || state_ == MeterState::NoSignal || now_ns <= last_ts_) return;
    if ((now_ns - last_ts_) / 1e9 > cfg_.no_signal_s) {
        state_ = MeterState::NoSignal;
        inst_w_ = 0;
        char msg[96];
        std::snprintf(msg, sizeof msg, "NO SIGNAL: no pulse for %.0f s (meter/line fault?)",
                      (now_ns - last_ts_) / 1e9);
        alert(now_ns, msg);
    }
}

Summary Analytics::make_summary() const {
    Summary s;
    s.pulses = pulses_;
    s.energy_kwh = energy_wh_ / 1000.0;
    s.inst_w = inst_w_;
    s.avg_w = avg_w_;
    s.peak_w = peak_w_;
    s.cost = cost_;
    s.state = state_;
    s.elapsed_s = have_first_ ? (last_ts_ - first_ts_) / 1e9 : 0.0;
    if (s.elapsed_s >= 10.0)
        s.projected_kwh_month = s.energy_kwh / s.elapsed_s * 3600.0 * 24.0 * 30.0;
    return s;
}

Summary Analytics::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return make_summary();
}

std::vector<std::string> Analytics::drain_alerts() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    out.swap(alerts_);
    return out;
}
