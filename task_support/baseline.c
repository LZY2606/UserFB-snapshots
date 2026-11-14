#include "tsf.h"
#include "fixture.h"
#include "alloc.h"
#include <math.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "BASELINE_FAIL line=%d %s\n", __LINE__, #x); return 1; } } while (0)

static tsf *load(int mode) {
    struct task_font_bytes b = task_make_font(mode);
    tsf *f = b.data ? tsf_load_memory(b.data, b.size) : NULL;
    free(b.data); return f;
}
int main(void) {
    enum { N = 4096 };
    float mono[N], stereo[N*2], split[N*2], continued[N], copied[N];
    tsf *f[3], *copy, *control; double energy = 0, maxerr = 0; int i;
    for (i = 0; i < 3; ++i) {
        f[i] = load(0); REQUIRE(f[i]); REQUIRE(tsf_get_presetcount(f[i]) == 1);
        tsf_set_output(f[i], i == 0 ? TSF_MONO : (i == 1 ? TSF_STEREO_INTERLEAVED : TSF_STEREO_UNWEAVED), 22050, 0);
        REQUIRE(tsf_set_max_voices(f[i], 1)); REQUIRE(tsf_note_on(f[i], 0, 60, 1));
    }
    task_alloc_reset();
    tsf_render_float(f[0], mono, N, 0); tsf_render_float(f[1], stereo, N, 0);
    tsf_render_float(f[2], split, N, 0);
    REQUIRE(task_alloc_calls() == 0);
    for (i = 0; i < N; ++i) {
        int pos = i < 496 ? i : 16+(i-496)%480;
        double expected = task_pcm(0, pos)/32767.0;
        double err = fabs(mono[i]-expected);
        if (err > maxerr) maxerr = err;
        energy += mono[i]*mono[i];
        REQUIRE(fabs(stereo[i*2]-mono[i]*sqrt(.5)) < 1e-6);
        REQUIRE(stereo[i*2] == split[i]); REQUIRE(stereo[i*2+1] == split[N+i]);
    }
    REQUIRE(maxerr < 1e-6); REQUIRE(energy > 1);
    printf("BASELINE_MONO frames=%d source_reference=1 layouts=3 allocations=0 maxerr=%.9g energy=%.9g\n", N, maxerr, energy);
    for (i = 0; i < 3; ++i) tsf_close(f[i]);
    control = load(0); REQUIRE(control); copy = tsf_copy(control); REQUIRE(copy);
    tsf_close(control); tsf_set_output(copy, TSF_MONO, 22050, 0);
    REQUIRE(tsf_note_on(copy, 0, 60, 1)); tsf_render_float(copy, copied, N, 0);
    for (i = 0; i < N; ++i) REQUIRE(copied[i] == mono[i]);
    tsf_note_off(copy, 0, 60);
    for (i = 0; i < 4; ++i) tsf_render_float(copy, continued, N, 0);
    REQUIRE(tsf_active_voice_count(copy) == 0); tsf_close(copy);
    printf("BASELINE_LIFETIME copy_after_original_close=1 release_complete=1\n");
    for (i = 1; i <= 2; ++i) {
        double lr = 0; int j;
        f[0] = load(i); REQUIRE(f[0]);
        tsf_set_output(f[0], TSF_STEREO_INTERLEAVED, 22050, 0);
        REQUIRE(tsf_note_on(f[0], 0, 60, 1)); tsf_render_float(f[0], stereo, N, 0);
        for (j = 0; j < N; ++j) lr += fabs(stereo[j*2]-stereo[j*2+1]);
        printf("LINKED_DIAGNOSTIC reference=%s lr_abs_difference=%.9g active=%d\n", i == 1 ? "left" : "right", lr, tsf_active_voice_count(f[0]));
        tsf_close(f[0]);
    }
    puts("BASELINE_OK current_production=1"); return 0;
}
