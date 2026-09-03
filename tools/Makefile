# Ferramentas de host. Não fazem parte do firmware e não entram em nenhum
# ambiente do PlatformIO: elas linkam o núcleo puro direto, que é possível
# porque src/core não tem nenhuma dependência de Arduino.
#
#     make -C tools
#     tools/recover_log FLIGHT007.BIN > voo7.csv
#     tools/replay FLIGHT007.BIN 1.0 0.00002

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I../ELE3km/src

# O recuperador precisa só do codec do log. O harness de replay realimenta o núcleo
# inteiro, então linka todos os .cpp de src/core (todos puros).
CORE := ../ELE3km/src/core/log_codec.cpp
CORE_ALL := $(wildcard ../ELE3km/src/core/*.cpp)

all: recover_log replay

recover_log: recover_log.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ $^

replay: replay.cpp $(CORE_ALL)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f recover_log replay

.PHONY: all clean
