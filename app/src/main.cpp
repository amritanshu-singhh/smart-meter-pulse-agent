// smeter_agent - Smart Energy Smart-Meter Pulse Counter & Analytics Agent
//
//   producer thread : waits for pulses (driver or simulator) -> EventQueue
//   consumer thread : EventQueue -> Analytics (+ CSV log, loss-of-signal check)
//   main thread     : prints a dashboard + alerts once per second
#include "analytics.hpp"
#include "dataset.hpp"
#include "event_queue.hpp"
#include "model.hpp"
#include "pulse_source.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <string>
#include <thread>
#include <unistd.h>

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

struct Options {
    std::string dev = "/dev/smeter0", csv, logfile = "/tmp/smeter_agent.log", replay, model;
    bool sim = false, outage = false, daemon = false;
    double speed = 1.0, window = 60.0, duration = 0.0, no_signal = -1.0;  // duration 0 = run until Ctrl+C
    uint32_t ppk = 1000;
};

static void usage(const char* p) {
    std::printf(
        "Usage: %s [options]\n"
        "  --dev PATH      character device (default /dev/smeter0)\n"
        "  --replay FILE   replay a dataset CSV (datetime,watts[,label]) as meter pulses\n"
        "  --model FILE    anomaly model trained by smeter_train (see data/ and models/)\n"
        "  --no-signal S   silence (s) before NO_SIGNAL alert (default 30, replay 120)\n"
        "  --sim           use the built-in simulator (no driver / root needed)\n"
        "  --speed N       simulator time acceleration (default 1)\n"
        "  --outage        simulator: inject a 50 s blackout (tests NO_SIGNAL)\n"
        "  --ppk N         simulator meter constant, imp/kWh (default 1000)\n"
        "  --window S      sliding window for average demand (default 60)\n"
        "  --duration S    stop after S wall-clock seconds\n"
        "  --csv FILE      append per-pulse analytics to a CSV file\n"
        "  --daemon        run in background, output goes to --log\n"
        "  --log FILE      daemon log file (default /tmp/smeter_agent.log)\n",
        p);
}

static bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); return nullptr; }
            return argv[++i];
        };
        const char* v = nullptr;
        if (a == "--sim") o.sim = true;
        else if (a == "--outage") o.outage = true;
        else if (a == "--daemon") o.daemon = true;
        else if (a == "-h" || a == "--help") return false;
        else if (a == "--dev")      { if (!(v = next("--dev"))) return false; o.dev = v; }
        else if (a == "--replay")   { if (!(v = next("--replay"))) return false; o.replay = v; }
        else if (a == "--model")    { if (!(v = next("--model"))) return false; o.model = v; }
        else if (a == "--no-signal"){ if (!(v = next("--no-signal"))) return false; o.no_signal = std::atof(v); }
        else if (a == "--csv")      { if (!(v = next("--csv"))) return false; o.csv = v; }
        else if (a == "--log")      { if (!(v = next("--log"))) return false; o.logfile = v; }
        else if (a == "--speed")    { if (!(v = next("--speed"))) return false; o.speed = std::atof(v); }
        else if (a == "--window")   { if (!(v = next("--window"))) return false; o.window = std::atof(v); }
        else if (a == "--duration") { if (!(v = next("--duration"))) return false; o.duration = std::atof(v); }
        else if (a == "--ppk")      { if (!(v = next("--ppk"))) return false; o.ppk = std::strtoul(v, nullptr, 10); }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return false; }
    }
    return true;
}

// Classic double-fork-free daemonisation: fork, setsid, redirect stdio.
static bool daemonize(const std::string& logfile) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid > 0) { std::printf("smeter_agent running in background (pid %d), log: %s\n", pid, logfile.c_str()); std::exit(0); }
    setsid();
    int fd = open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    int nul = open("/dev/null", O_RDONLY);
    if (fd < 0 || nul < 0) return false;
    dup2(nul, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
    close(nul); close(fd);
    return true;
}

static void print_dashboard(const Summary& s, double wall_s) {
    std::printf("[%4.0fs] pulses=%-5llu inst=%7.0f W  avg=%7.0f W  peak=%7.0f W  energy=%.4f kWh  cost=Rs %.3f  %s\n",
                wall_s, static_cast<unsigned long long>(s.pulses), s.inst_w, s.avg_w, s.peak_w,
                s.energy_kwh, s.cost, to_string(s.state));
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) { usage(argv[0]); return 1; }

    std::unique_ptr<PulseSource> src;
    try {
        if (!opt.replay.empty()) {
            std::vector<Sample> samples;
            std::string err;
            if (!load_dataset(opt.replay, samples, err)) throw std::runtime_error(err);
            src = std::make_unique<ReplaySource>(samples, opt.ppk, opt.speed);
        } else if (opt.sim) src = std::make_unique<SimSource>(opt.ppk, opt.speed, opt.outage);
        else         src = std::make_unique<DeviceSource>(opt.dev);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }

    if (opt.daemon && !daemonize(opt.logfile)) { std::perror("daemonize"); return 3; }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    AnalyticsConfig cfg;
    cfg.pulses_per_kwh = src->pulses_per_kwh();
    cfg.window_s = opt.window;
    cfg.no_signal_s = opt.no_signal > 0 ? opt.no_signal : (opt.replay.empty() ? 30.0 : 120.0);
    HourlyModel model;
    if (!opt.model.empty()) {
        if (!model.load(opt.model)) { std::fprintf(stderr, "error: cannot load model %s\n", opt.model.c_str()); return 5; }
        cfg.model = &model;
    }
    Analytics analytics(cfg, src->wall_start());
    EventQueue<PulseEvent> queue;

    std::ofstream csv;
    if (!opt.csv.empty()) {
        bool fresh = !std::ifstream(opt.csv).good();
        csv.open(opt.csv, std::ios::app);
        if (!csv) { std::fprintf(stderr, "cannot open %s\n", opt.csv.c_str()); return 4; }
        if (fresh) csv << "seq,ts_ns,inst_w,avg_w,energy_kwh,cost_rs,state\n";
    }

    std::printf("smeter_agent: source=%s, %u imp/kWh, window=%.0fs, detector=%s  (Ctrl+C to stop)\n",
                src->name().c_str(), cfg.pulses_per_kwh, cfg.window_s,
                cfg.model ? "trained hourly model" : "ratio-to-baseline");

    std::thread producer([&] {
        PulseEvent ev{};
        while (!g_stop) {
            int r = src->wait_event(ev, 200);
            if (r > 0) queue.push(ev);
            else if (r < 0) { std::fprintf(stderr, "read error from source\n"); g_stop = true; }
            else if (src->finished()) g_stop = true;   // finite source (dataset replay) exhausted
        }
    });

    std::thread consumer([&] {
        PulseEvent ev{};
        while (!g_stop || !queue.empty()) {
            if (queue.pop_for(ev, std::chrono::milliseconds(100))) {
                Summary s = analytics.on_pulse(ev.ts_ns);
                if (csv.is_open())
                    csv << ev.seq << ',' << ev.ts_ns << ',' << s.inst_w << ',' << s.avg_w << ','
                        << s.energy_kwh << ',' << s.cost << ',' << to_string(s.state) << '\n';
            }
            analytics.check_timeout(src->now_ns());
        }
    });

    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    double next_tick = 1.0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (opt.duration > 0 && elapsed() >= opt.duration) g_stop = true;
        for (const auto& a : analytics.drain_alerts()) { std::printf("  !! ALERT %s\n", a.c_str()); std::fflush(stdout); }
        if (elapsed() >= next_tick) { print_dashboard(analytics.snapshot(), elapsed()); next_tick += 1.0; }
    }

    producer.join();
    consumer.join();
    for (const auto& a : analytics.drain_alerts()) std::printf("  !! ALERT %s\n", a.c_str());

    const Summary s = analytics.snapshot();
    std::printf("\n=========== SESSION REPORT ===========\n"
                "pulses counted      : %llu\n"
                "energy consumed     : %.4f kWh\n"
                "peak demand (%.0fs)  : %.0f W\n"
                "estimated cost      : Rs %.3f (time-of-use tariff)\n"
                "projected / 30 days : %.1f kWh\n"
                "dropped events      : %zu\n"
                "======================================\n",
                static_cast<unsigned long long>(s.pulses), s.energy_kwh, opt.window, s.peak_w, s.cost,
                s.projected_kwh_month, queue.dropped());
    return 0;
}
