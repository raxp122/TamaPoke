CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -I../tests/stubs -I..

PET_TEST_OBJS = build/pet_tests.o build/pet.o build/species_chirp.o build/thumb_format.o build/audio_stub.o
BATTLE_TEST_OBJS = build/battle_tests.o build/battle.o
NET_TEST_OBJS = build/net_tests.o build/netlogic.o

.PHONY: test web-test clean

test: build/pet_tests build/battle_tests build/net_tests build/gesture_tests web-test
	./build/pet_tests
	./build/battle_tests
	./build/net_tests
	./build/gesture_tests

web-test:
	node web_serial_tests.mjs

build:
	mkdir -p build

build/pet_tests: $(PET_TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(PET_TEST_OBJS) -o $@

build/pet_tests.o: pet_tests.cpp ../pet.h ../dex.h | build
	$(CXX) $(CXXFLAGS) -c pet_tests.cpp -o $@

build/pet.o: ../pet.cpp ../pet.h ../dex.h ../audio.h | build
	$(CXX) $(CXXFLAGS) -c ../pet.cpp -o $@

build/species_chirp.o: ../species_chirp.cpp ../species_chirp.h ../dex.h | build
	$(CXX) $(CXXFLAGS) -c ../species_chirp.cpp -o $@

build/thumb_format.o: ../thumb_format.cpp ../thumb_format.h | build
	$(CXX) $(CXXFLAGS) -c ../thumb_format.cpp -o $@

build/audio_stub.o: audio_stub.cpp ../audio.h | build
	$(CXX) $(CXXFLAGS) -c audio_stub.cpp -o $@

build/battle_tests: $(BATTLE_TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(BATTLE_TEST_OBJS) -o $@

build/battle_tests.o: battle_tests.cpp ../battle.h | build
	$(CXX) $(CXXFLAGS) -c battle_tests.cpp -o $@

build/battle.o: ../battle.cpp ../battle.h | build
	$(CXX) $(CXXFLAGS) -c ../battle.cpp -o $@

build/net_tests: $(NET_TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(NET_TEST_OBJS) -o $@

build/net_tests.o: net_tests.cpp ../netlogic.h | build
	$(CXX) $(CXXFLAGS) -c net_tests.cpp -o $@

build/netlogic.o: ../netlogic.cpp ../netlogic.h | build
	$(CXX) $(CXXFLAGS) -c ../netlogic.cpp -o $@

build/gesture_tests: gesture_tests.cpp ../gesture.h | build
	$(CXX) $(CXXFLAGS) gesture_tests.cpp -o $@

clean:
	rm -rf build
