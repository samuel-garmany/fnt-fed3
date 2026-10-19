# Builds the device core for the PC, to test it against fnt's simulator.
#
#   make            build libfed3core.so
#   make test       build, then run fnt's device tests against it
#
# FNT points at an fnt checkout (default: ../../fnt next to this repo).

CXX ?= g++
CXXFLAGS ?= -std=c++14 -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC
FNT ?= $(abspath ../../fnt)
PYTHON ?= python3
SRC = ../core.cpp host_hal.cpp

libfed3core.so: $(SRC) ../core.h
	$(CXX) $(CXXFLAGS) -shared -o $@ $(SRC)

test: libfed3core.so
	FNT=$(FNT) $(PYTHON) run_sim_tests.py

clean:
	rm -f libfed3core.so

.PHONY: test clean
