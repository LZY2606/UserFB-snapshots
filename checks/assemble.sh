#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$1
{
 printf '/*\n'
 cat src/header_docs.txt
 printf '*/\n'
 cat src/header_macros_start.h src/buffer_format_macros.h src/crsw_math.h src/gl_types.h src/cvector_combined.h src/glcontext.h src/gl_glsl.h src/pgl_std_shaders.h src/gl_prototypes.h src/gl_stubs.h src/pgl_ext.h
 printf '\n#ifdef __cplusplus\n}\n#endif\n\n// end GL_H\n#endif\n\n'
 printf '#ifdef PORTABLEGL_IMPLEMENTATION\n\n'
 cat src/crsw_math.c src/cvector_combined.c src/gl_err.c src/gl_internal.c src/gl_tex_internal.c src/gl_fbo.c src/gl_impl.c src/gl_stubs.c src/gl_glsl.c src/pgl_ext.c src/pgl_std_shaders.c
 printf '#undef PORTABLEGL_IMPLEMENTATION\n#endif\n'
 cat src/close_pgl.h
} > "$out"
