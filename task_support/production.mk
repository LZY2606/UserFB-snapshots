CXX = c++
CC = cc
DIRS = liveMedia groupsock BasicUsageEnvironment UsageEnvironment
CPP = $(foreach d,$(DIRS),$(wildcard $(d)/*.cpp))
CSRC = $(foreach d,$(DIRS),$(wildcard $(d)/*.c))
OBJS = $(addprefix .task-build/,$(CPP:.cpp=.o) $(CSRC:.c=.o))
HEADERS = $(foreach d,$(DIRS),$(wildcard $(d)/include/*.hh $(d)/include/*.h))
FLAGS = -O0 -g -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t $(foreach d,$(DIRS),-I$(d)/include)
.task-build/production.a: $(OBJS)
	rm -f $@
	ar rcs $@ $(OBJS)
.task-build/%.o: %.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 $(FLAGS) -c $< -o $@
.task-build/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(FLAGS) -c $< -o $@
