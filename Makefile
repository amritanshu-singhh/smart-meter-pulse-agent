# Smart-Meter Pulse Counter & Analytics Agent - top-level build
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
INC      := -Iinclude -Iapp/src
BUILD    ?= build
BIN      := $(abspath $(BUILD))
KVER     := $(shell uname -r)
MODNAME  := driver/smeter_pulse.ko

LIB_SRC  := app/src/analytics.cpp app/src/pulse_source.cpp app/src/dataset.cpp app/src/model.cpp
HDRS     := $(wildcard app/src/*.hpp) include/smeter_ioctl.h
DATASET  := data/household_14d.csv
MODEL    := models/hourly_baseline.model

.PHONY: all app driver test data train load unload clean demo-sim demo-replay

all: app driver

app: $(BIN)/smeter_agent $(BIN)/smeter_ctl $(BIN)/smeter_gen_data $(BIN)/smeter_train

$(BIN)/smeter_agent: app/src/main.cpp $(LIB_SRC) $(HDRS)
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) app/src/main.cpp $(LIB_SRC) -o $@

$(BIN)/smeter_ctl: tools/smeter_ctl.cpp include/smeter_ioctl.h
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

$(BIN)/smeter_gen_data: tools/smeter_gen_data.cpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BIN)/smeter_train: tools/smeter_train.cpp app/src/dataset.cpp app/src/model.cpp $(HDRS)
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) tools/smeter_train.cpp app/src/dataset.cpp app/src/model.cpp -o $@

$(BIN)/test_all: tests/test_analytics.cpp $(LIB_SRC) $(HDRS)
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) tests/test_analytics.cpp $(LIB_SRC) -o $@

test: $(BIN)/test_all
	$(BIN)/test_all

# regenerate the synthetic dataset, then train the model on it (prints the evaluation)
data: $(BIN)/smeter_gen_data
	$(BIN)/smeter_gen_data $(DATASET) 14 2026

train: $(BIN)/smeter_train
	$(BIN)/smeter_train $(DATASET) $(MODEL) 10 | tee docs/training_report.txt

driver:
	$(MAKE) -C driver KDIR=/lib/modules/$(KVER)/build

load: driver
	sudo insmod $(MODNAME) pulses_per_kwh=1000 sim_load_w=1500 sim_enable=1
	sleep 1
	sudo chmod 666 /dev/smeter0

unload:
	-sudo rmmod smeter_pulse

demo-sim: app
	$(BIN)/smeter_agent --sim --speed 10 --outage --duration 30

# replay the whole 14-day dataset (~60 s at 20000x) through the agent with the trained model
demo-replay: app
	$(BIN)/smeter_agent --replay $(DATASET) --model $(MODEL) --speed 20000 --csv replay_out.csv

clean:
	-$(MAKE) -C driver clean KDIR=/lib/modules/$(KVER)/build
	rm -rf $(BUILD)
