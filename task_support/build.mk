CXX ?= c++
CC ?= cc
CPPFLAGS += -I. -Iexts/cgltf -Iexts/stb_image -Iexts/tinyexr -Iexts/fast_float/include -Iexts/nlohmann_json/single_include
CXXFLAGS += -std=c++17 -O0 -pthread -MMD -MP
CFLAGS += -std=c11 -O0 -MMD -MP
CORE = libs/yocto/yocto_shape.cpp libs/yocto/yocto_scene.cpp libs/yocto/yocto_bvh.cpp libs/yocto/yocto_image.cpp libs/yocto/yocto_sceneio.cpp libs/yocto/yocto_modelio.cpp libs/yocto/yocto_pbrtio.cpp exts/stb_image/stb_image/stb_image.cpp exts/tinyexr/tinyexr/tinyexr.cpp
CCORE = exts/cgltf/cgltf/cgltf.c exts/cgltf/cgltf/cgltf_write.c
OBJ = $(addprefix .task-build/,$(CORE:.cpp=.o) $(CCORE:.c=.o))
all: .task-build/baseline .task-build/historical .task-build/scene_baseline
.task-build/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
.task-build/baseline: task_support/baseline.cpp .task-build/libs/yocto/yocto_shape.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) task_support/baseline.cpp .task-build/libs/yocto/yocto_shape.o -o $@
.task-build/historical: task_support/historical_z1059.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@
.task-build/scene_baseline: task_support/scene_baseline.cpp $(OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) task_support/scene_baseline.cpp $(OBJ) -o $@
.task-build/target: task_support/driver.c checks/test_cover_subdiv.cpp $(OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ task_support/driver.c checks/test_cover_subdiv.cpp -x none $(OBJ) -o $@
-include $(OBJ:.o=.d) .task-build/baseline.d .task-build/historical.d .task-build/scene_baseline.d .task-build/target.d
.PHONY: all .task-build/target
