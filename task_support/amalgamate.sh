#!/bin/sh
set -eu
cd "$(dirname "$0")/../src"
{ printf '/*\n'; cat header_docs.txt; printf '*/\n'; cat header_macros_start.h buffer_format_macros.h crsw_math.h gl_types.h cvector_combined.h glcontext.h gl_glsl.h pgl_std_shaders.h gl_prototypes.h gl_stubs.h pgl_ext.h; printf '\n#ifdef __cplusplus\n}\n#endif\n#endif\n#ifdef PORTABLEGL_IMPLEMENTATION\n'; cat crsw_math.c cvector_combined.c gl_err.c gl_internal.c gl_tex_internal.c gl_fbo.c gl_impl.c gl_stubs.c gl_glsl.c pgl_ext.c pgl_std_shaders.c; printf '\n#undef PORTABLEGL_IMPLEMENTATION\n#endif\n'; cat close_pgl.h; } > ../.task-build/portablegl.h
