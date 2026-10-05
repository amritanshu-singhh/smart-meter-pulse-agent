#include "dataset.hpp"
#include <cctype>
#include <cstdio>
#include <fstream>

bool load_dataset(const std::string& path, std::vector<Sample>& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open dataset: " + path; return false; }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line[0]))) continue;  // header
        int Y, M, D, h, m, s, label = 0;
        double w = 0;
        int n = std::sscanf(line.c_str(), "%d-%d-%d %d:%d:%d,%lf,%d", &Y, &M, &D, &h, &m, &s, &w, &label);
        if (n < 7) { err = "bad dataset line: " + line; return false; }
        std::tm tmv{};
        tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
        tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s; tmv.tm_isdst = -1;
        out.push_back({std::mktime(&tmv), w < 0 ? 0 : w, n >= 8 ? label : 0});
    }
    if (out.size() < 2) { err = "dataset needs at least 2 samples"; return false; }
    return true;
}

std::string format_time(std::time_t t) {
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

int hour_of(std::time_t t) {
    std::tm tmv{};
    localtime_r(&t, &tmv);
    return tmv.tm_hour;
}
