set -euo pipefail
mkdir -p checks/.tmp
b=$(mktemp -d checks/.tmp/target.XXXXXX)
trap 'rm -rf "$b"' EXIT
test -s checks/target.c
for lang in c cpp; do
 if [ "$lang" = c ]; then
  cc -std=c11 -O2 -ffp-contract=off -DPGL_TILE_THREADS -DPGL_CHUNK_PRIMS=64 -I . checks/target.c checks/target_main.c -pthread -lm -o "$b/check"
 else
  c++ -x c++ -std=c++17 -O2 -ffp-contract=off -DPGL_TILE_THREADS -DPGL_CHUNK_PRIMS=64 -I . checks/target.c checks/target_main.c -pthread -lm -o "$b/check"
 fi
 "$b/check" | tee "$b/$lang.out"
 LC_ALL=C awk '
 /^GROUP_OK / {
   if (NF!=3 || $3 !~ /^[1-9][0-9]*$/ || final) bad=1;
   expected[1]="geometry"; expected[2]="ordering"; expected[3]="workers";
   if (++n>3 || $2!=expected[n]) bad=1;
   sum+=$3; next;
 }
 /^TARGET_OK / { if (NF!=2 || $2 !~ /^[1-9][0-9]*$/ || ++final!=1 || n!=3 || $2!=sum) bad=1; next; }
 END { if (bad || n!=3 || final!=1 || sum<=0) exit 1; }
 ' "$b/$lang.out"
done
test -s examples/tile_offscreen.c
cc -std=c11 -O2 -DPGL_TILE_THREADS -I . examples/tile_offscreen.c -pthread -lm -o "$b/demo"
"$b/demo" "$b/demo.ppm"
printf 'P6\n64 64\n255\n' > "$b/ppm-header"
head -c 13 "$b/demo.ppm" | cmp - "$b/ppm-header"
test "$(wc -c < "$b/demo.ppm" | tr -d ' ')" = 12301
bash checks/baseline.sh
bash checks/old-tests.sh
printf '%s\n' TARGET_COMPLETE
