CC=cc
CXX=c++
AR=ar
FLAGS=-Isrc/core -Isrc/core/lo -Itask_support/parser -I. -O1 -D__DISABLE_MIDI__ -D__DISABLE_HID__ -D__ALTER_HID__ -D__DISABLE_SERIAL__ -D__DISABLE_KBHIT__ -D__DISABLE_OTF_SERVER__ -D__DISABLE_NETWORK__ -D__DISABLE_SHELL__
CS=task_support/parser/chuck.tab.c task_support/parser/chuck.yy.c src/core/util_network.c src/core/util_raw.c src/core/util_xforms.c src/core/util_sndfile.c src/core/lo/address.c src/core/lo/blob.c src/core/lo/bundle.c src/core/lo/message.c src/core/lo/method.c src/core/lo/pattern_match.c src/core/lo/send.c src/core/lo/server.c src/core/lo/server_thread.c src/core/lo/timetag.c
CXXS=src/core/chuck_absyn.cpp src/core/chuck_parse.cpp src/core/chuck_errmsg.cpp src/core/chuck_frame.cpp src/core/chuck_symbol.cpp src/core/chuck_table.cpp src/core/chuck_vm.cpp src/core/chuck_instr.cpp src/core/chuck_scan.cpp src/core/chuck_type.cpp src/core/chuck_emit.cpp src/core/chuck_compile.cpp src/core/chuck_dl.cpp src/core/chuck_oo.cpp src/core/chuck_lang.cpp src/core/chuck_ugen.cpp src/core/chuck_stats.cpp src/core/chuck_io.cpp src/core/chuck.cpp src/core/chuck_carrier.cpp src/core/chuck_globals.cpp src/core/ugen_osc.cpp src/core/ugen_filter.cpp src/core/ugen_stk.cpp src/core/ugen_xxx.cpp src/core/ulib_ai.cpp src/core/ulib_doc.cpp src/core/ulib_machine.cpp src/core/ulib_math.cpp src/core/ulib_std.cpp src/core/ulib_opsc.cpp src/core/util_buffers.cpp src/core/util_console.cpp src/core/util_math.cpp src/core/util_platforms.cpp src/core/util_string.cpp src/core/util_thread.cpp src/core/util_opsc.cpp src/core/uana_xform.cpp src/core/uana_extract.cpp src/core/chuck_beat.cpp
# The mode-specific build directory is shared; keep the forced header and new
# source identical across target/baseline so switching modes cannot leave
# stale objects compiled with different preprocessor settings.
FORCE_HEADER=-include src/core/chuck_beat.h
OBJS=$(addprefix .task-build/,$(CS:.c=.o) $(CXXS:.cpp=.o))
all: $(OBJS)
	@rm -f .task-build/libchuck.a
	$(AR) rcs .task-build/libchuck.a $(OBJS)
.task-build/%.o: %.cpp task_support/flags.txt task_support/core.mk
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) $(FORCE_HEADER) -std=c++17 -MMD -MP -c $< -o $@
.task-build/%.o: %.c task_support/flags.txt task_support/core.mk
	@mkdir -p $(dir $@)
	$(CC) $(FLAGS) -MMD -MP -c $< -o $@
-include $(OBJS:.o=.d)
