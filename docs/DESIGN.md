# Design & UML

## Requirements
| ID | Requirement |
|----|-------------|
| R1 | Count meter pulses in the kernel with ns timestamps and no loss under normal load |
| R2 | Expose pulses and control to user space through a character device |
| R3 | Compute instantaneous/average/peak power, energy (kWh) and time-of-use cost |
| R4 | Detect load spikes and loss of signal and raise alerts |
| R5 | Log results to CSV, run in foreground or as a daemon, stop cleanly on SIGINT/SIGTERM |
| R6 | C/C++ only, Ubuntu only, documented on GitHub |

## Use-case diagram
```mermaid
flowchart LR
  U((Operator))
  U --> A[Monitor live power]
  U --> B[Receive spike / no-signal alerts]
  U --> C[Export CSV report]
  U --> D[Configure load & meter constant]
  M((Smart meter / simulator)) --> E[Generate pulses]
```

## Class diagram (user space)
```mermaid
classDiagram
  class PulseSource { <<abstract>> +wait_event() +now_ns() +pulses_per_kwh() }
  class ReplaySource { -ts_ }
  class HourlyModel { +zscore() +save() +load() }
  class DeviceSource { -int fd_ }
  class SimSource { -double speed_ }
  class EventQueue~T~ { +push() +pop_for() }
  class Analytics { +on_pulse() +check_timeout() +snapshot() +drain_alerts() }
  class Tariff { +rate_at(hour) }
  PulseSource <|-- DeviceSource
  PulseSource <|-- SimSource
  PulseSource <|-- ReplaySource
  Analytics o-- HourlyModel
  Analytics *-- Tariff
  EventQueue ..> Analytics : consumer feeds
```

## State machine (Analytics)
```mermaid
stateDiagram-v2
  [*] --> Idle
  Idle --> Normal : first pulse
  Normal --> Spike : inst > 2.5 x baseline
  Spike --> Normal : inst < 1.3 x baseline
  Normal --> NoSignal : silence > 30 s
  Spike --> NoSignal : silence > 30 s
  NoSignal --> Normal : pulse received
```

## Sequence diagram (one pulse)
```mermaid
sequenceDiagram
  participant T as hrtimer / S0 IRQ
  participant D as Driver (ring buffer)
  participant W as Workqueue
  participant P as Producer thread
  participant C as Consumer thread
  participant A as Analytics
  T->>D: smeter_record_pulse() (timestamp)
  D->>W: schedule_work() -> compute power
  D-->>P: wake_up / poll() ready
  P->>D: read(smeter_event)
  P->>C: EventQueue.push()
  C->>A: on_pulse(ts)
  A-->>C: Summary (+ alerts)
```

## Locking rules in the driver
- `sm.lock` (spinlock, `irqsave`): ring buffer and counters; taken from timer (hard IRQ) and process context.
- `sm.cfg_lock` (mutex): serialises ioctl configuration; never taken in atomic context.
- Power maths is deferred to a workqueue so the "interrupt" path stays short.
