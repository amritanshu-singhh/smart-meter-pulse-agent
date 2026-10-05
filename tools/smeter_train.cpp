// smeter_train - learns an hour-of-day baseline from a labelled dataset and evaluates it.
//
//   usage: smeter_train <dataset.csv> <model_out> [train_days=10]
//
// "Training" here means statistical learning, not a neural network:
//   1. mean/std of power for each hour of the day (from normal samples, label==0)
//   2. the z-score threshold that maximises F1 on the training days
//   3. evaluation (precision / recall / F1) on the held-out later days
// A single global threshold is trained the same way as a baseline to compare against.
#include "dataset.hpp"
#include "model.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

struct Score { int tp = 0, fp = 0, fn = 0, tn = 0; double p() const { return tp + fp ? double(tp) / (tp + fp) : 0; }
               double r() const { return tp + fn ? double(tp) / (tp + fn) : 0; }
               double f1() const { double a = p(), b = r(); return a + b > 0 ? 2 * a * b / (a + b) : 0; } };

using Detector = std::function<bool(const Sample&)>;

static Score evaluate(const std::vector<Sample>& s, size_t from, size_t to, const Detector& d) {
    Score sc;
    for (size_t i = from; i < to; ++i) {
        bool flag = d(s[i]), truth = s[i].label == 1;
        (flag ? (truth ? sc.tp : sc.fp) : (truth ? sc.fn : sc.tn))++;
    }
    return sc;
}

static void print(const char* name, const Score& s) {
    std::printf("  %-26s precision=%.3f recall=%.3f F1=%.3f  (TP=%d FP=%d FN=%d TN=%d)\n",
                name, s.p(), s.r(), s.f1(), s.tp, s.fp, s.fn, s.tn);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <dataset.csv> <model_out> [train_days=10]\n", argv[0]); return 1; }
    const int train_days = argc > 3 ? std::atoi(argv[3]) : 10;

    std::vector<Sample> data;
    std::string err;
    if (!load_dataset(argv[1], data, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }

    const std::time_t split_t = data.front().t + static_cast<std::time_t>(train_days) * 86400;
    size_t split = 0;
    while (split < data.size() && data[split].t < split_t) ++split;
    if (split == 0 || split >= data.size()) { std::fprintf(stderr, "error: bad train/test split\n"); return 3; }

    std::printf("dataset : %s (%zu samples, %s .. %s)\n", argv[1], data.size(),
                format_time(data.front().t).c_str(), format_time(data.back().t).c_str());
    std::printf("split   : train %zu samples (first %d days) | test %zu samples (held out)\n",
                split, train_days, data.size() - split);

    // 1. hour-of-day statistics from normal training samples
    double sum[24] = {}, sq[24] = {}, gsum = 0, gsq = 0;
    long cnt[24] = {}, gcnt = 0;
    for (size_t i = 0; i < split; ++i) {
        if (data[i].label) continue;
        int h = hour_of(data[i].t);
        sum[h] += data[i].watts; sq[h] += data[i].watts * data[i].watts; ++cnt[h];
        gsum += data[i].watts; gsq += data[i].watts * data[i].watts; ++gcnt;
    }
    HourlyModel model;
    for (int h = 0; h < 24; ++h) {
        if (!cnt[h]) { std::fprintf(stderr, "error: no training data for hour %d\n", h); return 4; }
        model.mean[h] = sum[h] / cnt[h];
        model.stdv[h] = std::sqrt(std::max(0.0, sq[h] / cnt[h] - model.mean[h] * model.mean[h]));
    }
    const double gmean = gsum / gcnt, gstd = std::sqrt(gsq / gcnt - gmean * gmean);

    // 2. pick thresholds that maximise F1 on the training days
    double best_z = 0, best_f1 = -1, best_gz = 0, best_gf1 = -1;
    for (double z = 2.0; z <= 20.0 + 1e-9; z += 0.5) {
        Score a = evaluate(data, 0, split, [&](const Sample& s) { return model.zscore(hour_of(s.t), s.watts) > z; });
        if (a.f1() > best_f1) { best_f1 = a.f1(); best_z = z; }
        Score g = evaluate(data, 0, split, [&](const Sample& s) { return (s.watts - gmean) / gstd > z; });
        if (g.f1() > best_gf1) { best_gf1 = g.f1(); best_gz = z; }
    }
    model.z_threshold = best_z;
    model.valid = true;

    std::printf("\nlearned hourly baseline (normal usage, watts):\n  hour  mean    std\n");
    for (int h = 0; h < 24; ++h) std::printf("  %02d   %6.1f  %6.1f\n", h, model.mean[h], model.stdv[h]);
    std::printf("\nchosen z-threshold: %.1f (hourly model) | %.1f (global baseline)\n", best_z, best_gz);

    // 3. evaluation
    auto hourly = [&](const Sample& s) { return model.zscore(hour_of(s.t), s.watts) > best_z; };
    auto global = [&](const Sample& s) { return (s.watts - gmean) / gstd > best_gz; };
    std::printf("\nTRAIN days:\n");
    print("hourly model (ours)", evaluate(data, 0, split, hourly));
    print("single global threshold", evaluate(data, 0, split, global));
    std::printf("TEST days (held out, never seen in training):\n");
    print("hourly model (ours)", evaluate(data, split, data.size(), hourly));
    print("single global threshold", evaluate(data, split, data.size(), global));

    if (!model.save(argv[2])) { std::fprintf(stderr, "error: cannot write %s\n", argv[2]); return 5; }
    std::printf("\nmodel saved to %s\n", argv[2]);
    return 0;
}
