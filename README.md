# Smart Energy Smart-Meter Pulse Counter & Analytics Agent

A Linux project in **C / C++ only** that models how a smart energy meter works:
the meter emits one electrical pulse (S0 / LED impulse) for every fixed amount of
energy (e.g. 1000 imp/kWh = 1 Wh per pulse). A **kernel character-device driver**
counts and timestamps those pulses, and a **multithreaded C++ user-space agent**
turns them into live power, energy, cost and anomaly alerts. A **dataset** can be
replayed as meter pulses and a **baseline model is trained on it** to detect abnormal consumption.

> Capstone for the Wipro Embedded Track (Linux System Programming + Linux Device Drivers + C++ + Software Architecture).

## Features

| Part | What it does |
|------|--------------|
| Kernel driver `smeter_pulse.ko` | `/dev/smeter0` char device; ns timestamps; ring buffer; blocking/non-blocking `read`, `poll`, `ioctl`; sysfs attributes; built-in pulse simulator (hrtimer) because a PC has no real S0 input |
| Agent `smeter_agent` | Instantaneous / average / peak power, kWh, time-of-use cost (Rs), 30-day projection, anomaly detection, loss-of-signal detection, CSV log, daemon mode |
| Sources | kernel driver, built-in simulator (`--sim`), or **dataset replay** (`--replay`) |
| `smeter_gen_data` | generates the 14-day dataset in `data/` |
| `smeter_train` | trains the hourly baseline model on the dataset and evaluates it on held-out days |
| `smeter_ctl` | set load / meter constant, start/stop simulator, inject pulses, read stats (ioctl + write) |
| Tests | unit tests (analytics, model, dataset replay): `make test` |

Physics: `P[W] = (1000 / pulses_per_kWh) [Wh] x 3600 / dt[s]`.

## Dataset and training

`data/household_14d.csv` is **synthetic** (see `data/README.md`): 14 days, 1-minute samples, with 28 labelled anomalies.

"Training" is statistical learning in C++ (no neural network, no Python):
1. learn mean and standard deviation of power for each hour of the day from normal samples (first 10 days),
2. pick the z-score threshold with the best F1 on those days (searched 2.0 - 20.0),
3. evaluate on the last 4 days, which were not used in training.

```bash
make train      # prints the report and saves models/hourly_baseline.model (also docs/training_report.txt)
```

Result of the included run (held-out days 11-14):

| Detector | Precision | Recall | F1 |
|----------|-----------|--------|----|
| Hourly learned model (z > 9.5) | 0.844 | 1.000 | 0.916 |
| Single global threshold (baseline) | 0.950 | 1.000 | 0.974 |

All 76 anomalous minutes were found (recall 1.0). The simple global threshold is slightly **better** on this
dataset: the injected anomalies (+2.8 kW or more) are far above normal usage at any hour, so a per-hour model
adds little, and normal kettle-type bursts at night raise its false alarms. The hourly model would matter more for
subtle anomalies (e.g. +500 W at 3 AM). The model is used by the agent at run time:

```bash
make demo-replay    # replays the 14 days (~60 s) through the agent with the trained model
```
The replayed energy matches the dataset exactly (191.953 kWh); the agent raised 37 anomaly alerts for the 28 injected events
(extra alerts come from normal bursts, see the table above).

## Architecture

```
 (real meter S0 pulse)  or  hrtimer simulator                  KERNEL SPACE
              |  "interrupt"
              v
     smeter_record_pulse()  --timestamp-->  ring buffer (spinlock_irqsave)
              |  schedule_work()                      |  wake_up / poll
              v                                       |
   workqueue: compute power (bottom half)             |
 ------------------------------------------------------------------------
                                        /dev/smeter0  |             USER SPACE
                                                      v
 PulseSource (DeviceSource | SimSource | ReplaySource) -> [producer thread]
                                  -> EventQueue (mutex+condvar) -> [consumer thread]
                                  -> Analytics (+ trained HourlyModel) -> CSV
                                  [main thread] dashboard + alerts
```
UML (use-case, class, state machine, sequence) is in [`docs/DESIGN.md`](docs/DESIGN.md).

## Syllabus concepts covered

| Syllabus module | Where in the project |
|-----------------|----------------------|
| Computer Architecture - Hardware | pulse = interrupt, top/bottom half, ns timestamps, I/O path |
| Linux OS & Git | Ubuntu only, Makefile build, Git repo |
| C++ Programming | classes, RAII, abstract class + virtual functions (`PulseSource`), templates (`EventQueue<T>`), STL, `std::thread`/mutex/condvar, exceptions, file streams, `unique_ptr`, Makefile/g++ |
| Linux System Programming | file descriptors, `poll`/`read`/`ioctl`/`write`, `sigaction`, daemon (`fork`/`setsid`), `clock_gettime` |
| Linux Device Drivers | module params, `printk`, `cdev`, `file_operations`, mutex + spinlock, hrtimer, workqueue, wait queue/poll, `copy_to/from_user`, class + sysfs |
| Computer Architecture - Software | layered design, producer/consumer, strategy pattern, SOLID |
| SDLC & Agile (UML) | requirements and UML in `docs/DESIGN.md` |

## Requirements (Ubuntu 22.04 / 24.04, also works in a VM)

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
```

## Build

```bash
make            # C++ apps/tools + kernel module
make test       # unit tests
```

## Run

### A) Without the driver (no root)
```bash
make app
./build/smeter_agent --sim --speed 10 --outage --duration 40 --csv run.csv       # simulator
./build/smeter_agent --replay data/household_14d.csv --model models/hourly_baseline.model --speed 20000
```

### B) With the kernel driver (full project)
```bash
make load                         # insmod + creates /dev/smeter0
dmesg | tail -3                   # "smeter: loaded, /dev/smeter0 ..."
./build/smeter_agent --duration 60 --model models/hourly_baseline.model --csv run.csv

# second terminal:
./build/smeter_ctl stats
./build/smeter_ctl load 5000      # a 5 kW appliance switches on -> alert
./build/smeter_ctl load 800
./build/smeter_ctl pulse 5        # inject 5 manual pulses
cat /sys/class/smeter/smeter0/total_pulses
make unload
```
Background mode: `./build/smeter_agent --daemon --log /tmp/agent.log`, stop with `pkill smeter_agent`.

### Agent options
`--dev PATH`, `--sim`, `--speed N`, `--outage`, `--ppk N`, `--replay FILE`, `--model FILE`, `--no-signal S`,
`--window S`, `--duration S`, `--csv FILE`, `--daemon`, `--log FILE`.

## Project layout

```
include/smeter_ioctl.h     shared kernel/user ABI
driver/smeter_pulse.c      kernel module (+ kbuild Makefile)
app/src/analytics.*        analytics engine (pure logic, unit-tested)
app/src/model.*            hourly baseline model (save/load/z-score)
app/src/dataset.*          CSV dataset loader
app/src/pulse_source.*     DeviceSource / SimSource / ReplaySource
app/src/event_queue.hpp    thread-safe queue template
app/src/main.cpp           agent: threads, signals, daemon, dashboard
tools/                     smeter_ctl, smeter_gen_data, smeter_train
data/                      synthetic 14-day dataset
models/                    trained model
tests/                     unit tests
docs/                      DESIGN.md (UML), training_report.txt
```

## Troubleshooting

- `insmod: Key was rejected` - Secure Boot blocks unsigned modules; disable it or use a VM.
- `make driver` fails - install headers: `sudo apt install linux-headers-$(uname -r)`.
- `cannot open /dev/smeter0` - run `make load`, or use `--sim` / `--replay`.

## Limitations / future work
Pulses on a PC are simulated (hrtimer); on real hardware call `smeter_record_pulse()` from a GPIO IRQ handler.
The dataset is synthetic. Future ideas: median/MAD robust statistics, per-weekday profiles, MQTT export.
