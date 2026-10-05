// smeter_gen_data - generates a synthetic household consumption dataset (1-minute samples).
// Output CSV: datetime,watts,label   (label=1 marks injected anomalies)
//   usage: smeter_gen_data <out.csv> [days=14] [seed=2026]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <vector>

// typical daily shape in watts, one anchor per hour (linear interpolation)
static const double kShape[24] = {120, 115, 110, 110, 115, 130, 300, 700, 650, 350, 300, 300,
                                  320, 300, 300, 310, 330, 500, 900, 1300, 1500, 1100, 500, 220};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <out.csv> [days] [seed]\n", argv[0]); return 1; }
    const int days = argc > 2 ? std::atoi(argv[2]) : 14;
    const unsigned seed = argc > 3 ? std::atoi(argv[3]) : 2026;
    const int n = days * 1440;
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    std::vector<double> add(n, 0.0);   // appliance bursts + anomalies
    std::vector<int> label(n, 0);

    // normal short appliance bursts (kettle, mixer, iron...), rarer at night
    for (int i = 0; i < n; ++i) {
        int hour = (i % 1440) / 60;
        double p = (hour < 6) ? 0.002 : 0.010;
        if (uni(rng) < p) {
            double w = 800 + 700 * uni(rng);
            int len = 2 + static_cast<int>(3 * uni(rng));
            for (int k = 0; k < len && i + k < n; ++k) add[i + k] += w;
        }
    }
    // injected anomalies: ~2 per day, 5-15 minutes, +2800..4500 W (e.g. faulty heater / leakage)
    for (int d = 0; d < days; ++d) {
        for (int e = 0; e < 2; ++e) {
            int start = d * 1440 + static_cast<int>(uni(rng) * (1440 - 20));
            int len = 5 + static_cast<int>(uni(rng) * 10);
            double w = 2800 + 1700 * uni(rng);
            for (int k = 0; k < len && start + k < n; ++k) { add[start + k] += w; label[start + k] = 1; }
        }
    }

    std::tm t0{};
    t0.tm_year = 2026 - 1900; t0.tm_mon = 8; t0.tm_mday = 1; t0.tm_isdst = -1;   // 2026-09-01 (Tuesday)
    std::time_t start = std::mktime(&t0);

    std::FILE* f = std::fopen(argv[1], "w");
    if (!f) { std::perror(argv[1]); return 2; }
    std::fprintf(f, "datetime,watts,label\n");
    for (int i = 0; i < n; ++i) {
        std::time_t t = start + static_cast<std::time_t>(i) * 60;
        std::tm tmv{};
        localtime_r(&t, &tmv);
        double hf = tmv.tm_hour + tmv.tm_min / 60.0;
        int h0 = static_cast<int>(hf) % 24, h1 = (h0 + 1) % 24;
        double base = kShape[h0] + (kShape[h1] - kShape[h0]) * (hf - std::floor(hf));
        bool weekend = (tmv.tm_wday == 0 || tmv.tm_wday == 6);
        if (weekend && tmv.tm_hour >= 8) base *= 1.2;
        double fridge = ((i % 40) < 10) ? 80.0 : 0.0;
        double w = base + fridge + add[i] + gauss(rng) * (0.06 * base + 8.0);
        if (w < 100) w = 100;
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
        std::fprintf(f, "%s,%.1f,%d\n", buf, w, label[i]);
    }
    std::fclose(f);
    std::printf("wrote %d samples (%d days) to %s\n", n, days, argv[1]);
    return 0;
}
