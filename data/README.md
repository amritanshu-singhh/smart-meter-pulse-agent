# Dataset

`household_14d.csv` - 14 days (2026-09-01 .. 2026-09-14) of household power, one sample per minute
(20,160 rows). Columns: `datetime,watts,label`.

**This dataset is synthetic.** It is produced by `tools/smeter_gen_data.cpp` (seed 2026) so the
project is reproducible offline. It contains
- a daily usage shape (night low, morning and evening peaks, weekends ~20% higher at day time),
- a fridge cycle and random short appliance bursts (kettle, mixer; `label=0`, they are normal),
- 28 injected anomalies (2 per day, 5-15 min, +2800..4500 W) marked `label=1`.

Regenerate with `make data`. To use real data, supply any CSV with the same columns
(`label` is optional), e.g. a resampled export of the UCI "Individual household electric power
consumption" dataset; no code change is needed.
