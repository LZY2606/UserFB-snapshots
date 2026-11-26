// Optional fragment execution tracing: instruments an optimized fragment
// NIR shader with a per-pixel event log written to a new std430 SSBO.
//
// The pass runs after the regular optimization pipeline. It first builds a
// stable catalog of trace sites on the original optimized NIR, and only then
// inserts the logging code. Instructions created here are never catalogued.
#pragma once

#include "glsl_optimizer.h"

struct nir_shader;

// Instruments a GLSL 430 fragment shader according to config. On success fills
// info (strings and arrays allocated in mem_ctx) and returns true. On a
// configuration/Shader limitation failure returns false and sets *error to a
// ralloc_strdup'd message in mem_ctx.
bool glslopt_trace_fragment(nir_shader* nir, const glslopt_trace_config& config,
                            glslopt_trace_info* info, void* mem_ctx, const char** error);
