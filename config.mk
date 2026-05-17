PREFIX    = /usr
MANPREFIX = $(PREFIX)/share/man

CC = c99

COMMON_SANITIZE = -fsanitize=alignment,shift,signed-integer-overflow,object-size,null,undefined,bounds,address
CLANG_SANITIZE  = -O1 $(COMMON_SANITIZE),cfi -flto -fvisibility=hidden -fno-sanitize-trap=cfi
GCC_SANITIZE    = -O1 $(COMMON_SANITIZE)
#SANITIZE        = $(CLANG_SANITIZE)
#SANITIZE        = $(GCC_SANITIZE)

CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700
CFLAGS   = $(SANITIZE) -O3
LDFLAGS  = $(SANITIZE)

# You can add -DALLOCA_LIMIT=# to CPPFLAGS, where # is a size_t
# value, to put a limit on how large allocation the library is
# allowed to make with alloca(3). For buffers that can have any
# size this limit will be used if it wants to allocate a larger
# buffer. Choose 0 to use malloc(3) instead of alloca(3).
