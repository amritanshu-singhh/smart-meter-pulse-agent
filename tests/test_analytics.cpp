// Unit tests for the analytics engine (no hardware needed): `make test`
#include "analytics.hpp"
#include "dataset.hpp"
#include "model.hpp"
#include "pulse_source.hpp"
#include <fstream>
#include <cassert>
#include <cmath>
#include <cstdio>

static const uint64_t S = 1000000000ULL;
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void test_constant_load() {
    AnalyticsConfig c;  // 1000 imp/kWh -> 1 Wh/pulse; one pulse per second = 3600 W
    Analytics a(c, 0);
    Summary s;
    for (int i = 1; i <= 11; ++i) s = a.on_pulse(i * S);
    assert(s.pulses == 11);
    assert(near(s.energy_kwh, 0.011, 1e-9));
    assert(near(s.inst_w, 3600, 1e-6));
    assert(near(s.avg_w, 3600, 1e-6));
    assert(s.state == MeterState::Normal);
    assert(a.drain_alerts().empty());
    std::puts("ok constant load");
}

static void test_spike_and_recovery() {
    AnalyticsConfig c;
    Analytics a(c, 0);
    uint64_t t = 0;
    for (int i = 0; i < 12; ++i) a.on_pulse(t += S);          // baseline 3600 W
    Summary s = a.on_pulse(t += S / 5);                       // 18000 W
    assert(s.state == MeterState::Spike);
    assert(a.drain_alerts().size() == 1);
    s = a.on_pulse(t += S);                                   // back to 3600 W
    assert(s.state == MeterState::Normal);
    assert(a.drain_alerts().size() == 1);
    std::puts("ok spike detection + recovery");
}

static void test_no_signal() {
    AnalyticsConfig c;
    Analytics a(c, 0);
    uint64_t t = 0;
    for (int i = 0; i < 5; ++i) a.on_pulse(t += S);
    a.check_timeout(t + 10 * S);
    assert(a.snapshot().state == MeterState::Normal);
    a.check_timeout(t + 31 * S);
    assert(a.snapshot().state == MeterState::NoSignal);
    a.on_pulse(t + 40 * S);
    assert(a.snapshot().state == MeterState::Normal);
    assert(a.drain_alerts().size() == 2);
    std::puts("ok loss of signal + restore");
}

static void test_tariff() {
    Tariff t;
    assert(t.rate_at(19) == t.peak);
    assert(t.rate_at(23) == t.offpeak);
    assert(t.rate_at(3) == t.offpeak);
    assert(t.rate_at(12) == t.normal);
    std::puts("ok tariff slabs");
}

static void test_replay_conserves_energy() {
    std::vector<Sample> s;
    std::time_t t0 = 1700000000;
    for (int i = 0; i < 10; ++i) s.push_back({t0 + i * 60, 1200.0, 0});   // 10 min @ 1200 W = 200 Wh
    for (int i = 10; i < 20; ++i) s.push_back({t0 + i * 60, 600.0, 0});   // 10 min @  600 W = 100 Wh
    ReplaySource r(s, 1000, 1.0);                                          // 1 Wh per pulse
    assert(r.pulse_count() >= 299 && r.pulse_count() <= 300);
    std::puts("ok replay conserves energy");
}

static void test_model_io_and_detection() {
    HourlyModel m;
    for (int h = 0; h < 24; ++h) { m.mean[h] = 3600; m.stdv[h] = 100; }
    m.z_threshold = 4; m.valid = true;
    assert(m.save("/tmp/smeter_test.model"));
    HourlyModel m2;
    assert(m2.load("/tmp/smeter_test.model") && m2.z_threshold == 4);
    assert(m2.zscore(5, 4000) > 3.9 && m2.zscore(5, 3600) == 0);

    AnalyticsConfig c;
    c.model = &m2;
    Analytics a(c, 0);
    uint64_t t = 0;
    for (int i = 0; i < 12; ++i) a.on_pulse(t += S);                 // 3600 W = learned normal
    assert(a.snapshot().state == MeterState::Normal);
    Summary s = a.on_pulse(t += S / 5);                              // 18000 W -> z = 144
    assert(s.state == MeterState::Spike);
    s = a.on_pulse(t += S);
    assert(s.state == MeterState::Normal);
    std::puts("ok model save/load + anomaly detection");
}

int main() {
    test_replay_conserves_energy();
    test_model_io_and_detection();
    test_constant_load();
    test_spike_and_recovery();
    test_no_signal();
    test_tariff();
    std::puts("ALL TESTS PASSED");
    return 0;
}
