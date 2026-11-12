# Reference echo cancellation demo - real SpeexDSP AEC integration.
# SpeexDSP is built locally under third_party/speexdsp (no system install).

SPEEX_PREFIX ?= third_party/speexdsp
CXX ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17
CPPFLAGS := -I$(SPEEX_PREFIX)/include -DAEC_DEMO_SPEEXDSP_VERSION='"1.2.1"' \
            -DAEC_DEMO_SERVICE_VERSION='"1.0.0"'
LDLIBS := -L$(SPEEX_PREFIX)/lib -Wl,-rpath,$(abspath $(SPEEX_PREFIX)/lib) \
          -lspeexdsp -lpthread

SRC := src/main.cpp src/wav_io.cpp src/signal_gen.cpp src/aec_processor.cpp \
       src/delay_adapter.cpp src/metrics.cpp src/json.cpp src/pipeline.cpp \
       src/http_server.cpp
OBJ := $(SRC:.cpp=.o)
BIN := aec_demo
TEST_BIN := /tmp/aec_test_contracts
DELAY_EXP_BIN := /tmp/aec_delay_experiment

.PHONY: all clean speex test accept delay-exp

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Delay-adapter comparison experiment (fixed vs constrained dynamic tracking),
# followed by an independent Python re-derivation of every reported metric.
delay-exp: $(DELAY_EXP_BIN)
	$(DELAY_EXP_BIN)
	python3 -I tests/verify_delay_experiment.py

verify-delay: delay-exp

$(DELAY_EXP_BIN): tests/delay_experiment.cpp $(filter-out src/main.o,$(OBJ))
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/delay_experiment.cpp \
	  $(filter-out src/main.o,$(OBJ)) -o $@ $(LDLIBS)

# Compile-time + processing-contract unit tests.
test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/test_contracts.cpp $(filter-out src/main.o,$(OBJ))
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/test_contracts.cpp \
	  $(filter-out src/main.o,$(OBJ)) -o $@ $(LDLIBS)

# End-to-end acceptance: all scenarios plus independent WAV re-verification.
accept: $(BIN)
	tests/acceptance.sh

# Convenience target if the vendored library needs rebuilding.
speex:
	cd third_party/build-speex && $(MAKE) -j4 && $(MAKE) install

clean:
	rm -f $(OBJ) $(BIN)
