CC := cc
CXX := c++
CPPFLAGS := -I. -Isrc -Itest -Itest/cutest
CFLAGS := -std=c99 -O0
CXXFLAGS := -std=c++11 -O0
.task-build/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
.task-build/%.cxx.o: %.cxx
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
.task-build/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
-include $(shell find .task-build -name '*.d' 2>/dev/null)
