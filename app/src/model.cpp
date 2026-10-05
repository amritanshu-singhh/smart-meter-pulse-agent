#include "model.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

bool HourlyModel::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "# smeter hourly baseline model (learned by smeter_train)\n"
        << "z_threshold " << z_threshold << "\n"
        << "# hour mean_w std_w\n";
    char buf[64];
    for (int h = 0; h < 24; ++h) {
        std::snprintf(buf, sizeof buf, "%d %.3f %.3f\n", h, mean[h], stdv[h]);
        out << buf;
    }
    return static_cast<bool>(out);
}

bool HourlyModel::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    int seen = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "z_threshold") { ls >> z_threshold; continue; }
        int h = std::atoi(key.c_str());
        double m, s;
        if (h >= 0 && h < 24 && (ls >> m >> s)) { mean[h] = m; stdv[h] = s; ++seen; }
    }
    valid = (seen == 24);
    return valid;
}
