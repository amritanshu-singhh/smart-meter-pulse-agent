// smeter_ctl - small control utility for /dev/smeter0 (ioctl / write demo)
#include "smeter_ioctl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <thread>

static void usage() {
    std::puts("usage: smeter_ctl [-d DEV] <command>\n"
              "  stats              print driver statistics\n"
              "  watch              print statistics every second\n"
              "  load <watts>       set simulated load\n"
              "  ppk <n>            set meter constant (imp/kWh)\n"
              "  sim on|off         start/stop the in-kernel pulse simulator\n"
              "  pulse [n]          inject n manual pulses (default 1)\n"
              "  reset              clear counters");
}

static int print_stats(int fd) {
    smeter_stats st{};
    if (ioctl(fd, SMETER_IOC_GET_STATS, &st) < 0) { std::perror("GET_STATS"); return 1; }
    std::printf("pulses=%llu energy=%.3f Wh power=%.1f W peak=%.1f W ppk=%u sim=%u load=%u W dropped=%u\n",
                (unsigned long long)st.total_pulses, st.energy_mwh / 1000.0, st.power_mw / 1000.0,
                st.peak_power_mw / 1000.0, st.pulses_per_kwh, st.sim_enabled, st.sim_load_w, st.dropped);
    return 0;
}

int main(int argc, char** argv) {
    const char* dev = "/dev/" SMETER_DEV_NAME;
    int i = 1;
    if (i + 1 < argc && !std::strcmp(argv[i], "-d")) { dev = argv[i + 1]; i += 2; }
    if (i >= argc) { usage(); return 1; }
    const std::string cmd = argv[i++];

    int fd = open(dev, O_RDWR);
    if (fd < 0) { std::perror(dev); return 2; }
    int rc = 0;
    unsigned val = 0;

    if (cmd == "stats") rc = print_stats(fd);
    else if (cmd == "watch") { while (true) { print_stats(fd); std::this_thread::sleep_for(std::chrono::seconds(1)); } }
    else if (cmd == "reset") rc = ioctl(fd, SMETER_IOC_RESET) < 0;
    else if ((cmd == "load" || cmd == "ppk") && i < argc) {
        val = std::strtoul(argv[i], nullptr, 10);
        rc = ioctl(fd, cmd == "load" ? SMETER_IOC_SET_LOAD : SMETER_IOC_SET_PPK, &val) < 0;
    } else if (cmd == "sim" && i < argc) {
        val = !std::strcmp(argv[i], "on");
        rc = ioctl(fd, SMETER_IOC_SIM_ENABLE, &val) < 0;
    } else if (cmd == "pulse") {
        int n = (i < argc) ? std::atoi(argv[i]) : 1;
        for (int k = 0; k < n; ++k) if (write(fd, "p", 1) != 1) { rc = 1; break; }
    } else { usage(); rc = 1; }

    if (rc) std::perror("smeter_ctl");
    close(fd);
    return rc;
}
