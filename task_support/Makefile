CXX := c++
CPPFLAGS := -DPMP_SCALAR_TYPE_64 -I. -Isrc -Iexternal/eigen-5.0.1
CXXFLAGS := -std=c++20 -O0
.task-build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
-include $(shell find .task-build -name '*.d' 2>/dev/null)
