// dataset.hpp - load power-consumption datasets (CSV: datetime,watts[,label])
#pragma once
#include <ctime>
#include <string>
#include <vector>

struct Sample {
    std::time_t t;   // local time of the reading
    double watts;    // average power during the interval starting at t
    int label;       // 1 = known anomaly (optional column), else 0
};

// Format: "YYYY-MM-DD HH:MM:SS,watts[,label]"; a header line is skipped.
bool load_dataset(const std::string& path, std::vector<Sample>& out, std::string& err);
std::string format_time(std::time_t t);
int hour_of(std::time_t t);
