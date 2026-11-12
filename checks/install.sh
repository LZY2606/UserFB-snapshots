set -euo pipefail
command -v cc >/dev/null
command -v c++ >/dev/null
mkdir -p checks/.tmp
b=$(mktemp -d checks/.tmp/install.XXXXXX)
trap 'rm -rf "$b"' EXIT
cat > "$b/threads.c" <<'C'
#include <pthread.h>
static void* run(void* value) { return value; }
int main(void) { pthread_t a,b; int x,y; void* r; if(pthread_create(&a,0,run,&x)||pthread_create(&b,0,run,&y)) return 1; if(pthread_join(a,&r)||r!=&x) return 2; if(pthread_join(b,&r)||r!=&y) return 3; return 0; }
C
cc -std=c11 "$b/threads.c" -pthread -o "$b/threads"
"$b/threads"
printf '#include "portablegl.h"\nint main(){return 0;}\n' > "$b/header.cpp"
c++ -std=c++17 -I . "$b/header.cpp" -o "$b/header"
"$b/header"
printf '%s\n' TOOLCHAIN_PTHREAD_OK
