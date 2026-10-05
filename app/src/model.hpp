// model.hpp - learned hour-of-day baseline (mean/std of power per hour) + z-score threshold
#pragma once
#include <algorithm>
#include <string>

struct HourlyModel {
    double mean[24] = {};
    double stdv[24] = {};
    double z_threshold = 3.5;
    bool valid = false;

    // how many standard deviations above the learned normal for this hour
    double zscore(int hour, double watts) const {
        const int h = ((hour % 24) + 24) % 24;
        return (watts - mean[h]) / std::max(stdv[h], 10.0);   // floor std at 10 W
    }
    bool save(const std::string& path) const;
    bool load(const std::string& path);
};
