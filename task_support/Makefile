CC ?= cc
AR ?= ar
CPPFLAGS += -D_DEFAULT_SOURCE -I. -Ilibelf -Icommon -Itask_support/include -Itask_support/vendor/zlib
CFLAGS += -std=c11 -O1 -g -MMD -MP
SOURCES := $(wildcard libelf/*.c) $(wildcard task_support/generated/*.c) $(wildcard task_support/vendor/zlib/*.c)
OBJECTS := $(patsubst %.c,.task-build/obj/%.o,$(SOURCES))
.PHONY: all
all: .task-build/baseline
.task-build/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
.task-build/libelf.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)
.task-build/baseline: task_support/baseline.c task_support/fixture.c .task-build/libelf.a
	$(CC) $(CPPFLAGS) -std=c11 task_support/baseline.c task_support/fixture.c .task-build/libelf.a -o $@
-include $(OBJECTS:.o=.d)
