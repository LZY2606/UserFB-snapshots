The generated include files freeze the upstream parseinstrs.py decode output
at the pinned revision, with --32 --64 and no undocumented opcodes. Run scripts
use only a C11 compiler and Bash. Upstream Python tools remain unmodified but
are not invoked by the task commands. fd_path.h specifies declarations only;
fd_path.c and tests/test_path.c are intentionally absent until implementation.
