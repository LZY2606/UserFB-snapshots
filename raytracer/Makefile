# Builds the ray tracer against one scene file from build/.
# Usage:
#   make                              # Crystal Cavern, 480x360 preview
#   make SCENE=build/buildCrystalCavern.cpp   # full 1920x1080 render
#   make run                          # build then render (writes scene.png)
#   make clean

CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -fopenmp
SCENE    ?= build/buildCrystalCavernLow.cpp

SRC := raytracer.cpp \
       $(wildcard world/*.cpp utilities/*.cpp geometry/*.cpp cameras/*.cpp \
                  image/*.cpp samplers/*.cpp materials/*.cpp brdf/*.cpp \
                  lights/*.cpp tracers/*.cpp acceleration/*.cpp) \
       $(SCENE)

BIN := scene.exe

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

.PHONY: run clean
run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) scene.png
