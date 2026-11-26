// NIR pass implementing optional fragment execution tracing.
//
// The pass runs on the already optimized, out-of-SSA fragment NIR. It first
// walks the original control flow to build a stable catalog of trace sites,
// and only then inserts the logging instructions. Every event is written to
// a new std430 SSBO immediately as it executes, so records are observable
// before a discard; the original code is never placed inside the logging
// guards and keeps its exact control flow, outputs, discards and memory side
// effects.
#include "nir_trace.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include "util/ralloc.h"
#include "util/u_math.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <set>

namespace {

// One catalogued trace site plus the stable anchor at which its event is
// logged. Anchors are collected before any block is mutated, so they remain
// valid while instrumentation splits blocks around them.
struct trace_site {
   unsigned id;
   glslopt_trace_kind kind;
   unsigned components;
   unsigned block;
   const char* output_name; // ralloc'd in mem_ctx, NULL except output sites
   unsigned write_mask;

   nir_instr* instr = nullptr; // ALU / intrinsic anchor (NULL for branches)
   nir_if* nif = nullptr;      // branch anchor
};

struct tracer {
   nir_shader* nir;
   nir_function_impl* impl;
   const glslopt_trace_config& config;
   void* mem_ctx;

   std::vector<trace_site> sites;
   nir_variable* ssbo_var = nullptr;
   std::string type_name, instance_name;
   unsigned segment_words = 0;

   // entry-block values; computed once and dominating every logging site
   nir_def* in_region = nullptr;    // bool1
   nir_def* segment_base = nullptr; // int32 word index of the pixel segment

   tracer(nir_shader* s, const glslopt_trace_config& cfg, void* mc)
      : nir(s), config(cfg), mem_ctx(mc)
   {
      impl = nir_shader_get_entrypoint(s);
   }

   nir_deref_instr* array_deref(nir_builder* b, nir_def* word_index)
   {
      nir_deref_instr* var_d = nir_build_deref_var(b, ssbo_var);
      nir_deref_instr* field_d = nir_build_deref_struct(b, var_d, 0);
      return nir_build_deref_array(b, field_d, word_index);
   }

   nir_def* load_word(nir_builder* b, nir_def* index)
   {
      return nir_load_deref(b, array_deref(b, index));
   }

   void store_word(nir_builder* b, nir_def* index, nir_def* value)
   {
      nir_store_deref(b, array_deref(b, index), value, 1);
   }
};

// nir_op_infos is the sole authority: mov/vec/bcsel are typeless (uint output
// type), so generic moves and the tracer's own arithmetic never qualify.
static bool
is_float32_output_alu(const nir_alu_instr* alu)
{
   return alu->def.bit_size == 32 &&
          nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) == nir_type_float;
}

static nir_variable*
deref_variable(nir_intrinsic_instr* in, unsigned src)
{
   nir_deref_instr* d = nir_src_as_deref(in->src[src]);
   return d ? nir_deref_instr_get_variable(d) : nullptr;
}

static bool
targets_shader_out(nir_intrinsic_instr* in, unsigned src)
{
   nir_variable* v = deref_variable(in, src);
   return v && v->data.mode == nir_var_shader_out;
}

// Recursive control-flow walk assigning stable site IDs in execution order.
static void
catalog_cf(tracer& t, struct exec_list* list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         nir_block* block = nir_cf_node_as_block(node);
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr* alu = nir_instr_as_alu(instr);
               if (is_float32_output_alu(alu)) {
                  t.sites.push_back({(unsigned)t.sites.size() + 1, kGlslTraceAlu,
                                     alu->def.num_components, block->index, nullptr, 0, instr, nullptr});
               }
            } else if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
               switch (in->intrinsic) {
               case nir_intrinsic_store_deref:
                  if (targets_shader_out(in, 0)) {
                     nir_variable* out = deref_variable(in, 0);
                     unsigned mask = nir_intrinsic_write_mask(in);
                     t.sites.push_back({(unsigned)t.sites.size() + 1, kGlslTraceOutput,
                                        (unsigned)util_bitcount(mask), block->index,
                                        ralloc_strdup(t.mem_ctx, out->name ? out->name : ""), mask,
                                        instr, nullptr});
                  }
                  break;
               case nir_intrinsic_copy_deref:
                  if (targets_shader_out(in, 0)) {
                     nir_variable* out = deref_variable(in, 0);
                     const glsl_type* ty = nir_src_as_deref(in->src[0])->type;
                     unsigned n = glsl_type_is_vector(ty) ? glsl_get_vector_elements(ty) : 1;
                     t.sites.push_back({(unsigned)t.sites.size() + 1, kGlslTraceOutput, n, block->index,
                                        ralloc_strdup(t.mem_ctx, out->name ? out->name : ""),
                                        nir_component_mask(n), instr, nullptr});
                  }
                  break;
               case nir_intrinsic_terminate:
               case nir_intrinsic_demote:
                  t.sites.push_back({(unsigned)t.sites.size() + 1, kGlslTraceDiscard, 0,
                                     block->index, nullptr, 0, instr, nullptr});
                  break;
               case nir_intrinsic_terminate_if:
               case nir_intrinsic_demote_if:
                  t.sites.push_back({(unsigned)t.sites.size() + 1, kGlslTraceDiscard, 0,
                                     block->index, nullptr, 0, instr, nullptr});
                  break;
               default:
                  break;
               }
            }
         }
         break;
      }
      case nir_cf_node_if: {
         nir_if* nif = nir_cf_node_as_if(node);
         // The condition is evaluated by the block immediately preceding the
         // if, after that block's instructions (already catalogued).
         unsigned block_index = nir_cf_node_as_block(nir_cf_node_prev(&nif->cf_node))->index;
         t.sites.push_back({(unsigned)t.sites.size() + 1, kGlslTraceBranch, 1, block_index,
                            nullptr, 0, nullptr, nif});
         catalog_cf(t, &nif->then_list);
         catalog_cf(t, &nif->else_list);
         break;
      }
      case nir_cf_node_loop:
         catalog_cf(t, &nir_cf_node_as_loop(node)->body);
         break;
      default:
         break;
      }
   }
}

// Builds the scalar 32-bit payload words for one event into words[]. It runs
// before any guard control flow is inserted, so every payload value dominates
// the logging block that consumes it.
typedef std::function<void(nir_builder* b, nir_def* words[4])> payload_builder;

// Emit one fully-formatted event. The payload builder materializes the
// component words first; the remaining payload words are zero. extra_guard
// (NULL for unconditional events) further restricts logging (conditional
// discard). The original instructions are never placed inside these guards.
static void
emit_event(tracer& t, nir_cursor at, const trace_site& site, unsigned components,
           const payload_builder& build_payload, nir_def* extra_guard)
{
   nir_builder b = nir_builder_at(at);

   // Materialize all payload values in the predecessor block first.
   nir_def* payload[4] = {};
   build_payload(&b, payload);

   nir_def* guard = t.in_region;
   if (extra_guard)
      guard = nir_iand(&b, guard, extra_guard);

   nir_if* region_if = nir_push_if(&b, guard);
   {
      nir_def* base = t.segment_base;
      nir_def* attempted = t.load_word(&b, base);
      // every executed event increments attempted first
      t.store_word(&b, base, nir_iadd_imm(&b, attempted, 1));

      nir_if* room_if = nir_push_if(&b, nir_ult_imm(&b, attempted, t.config.capacity));
      {
         nir_def* record = nir_iadd(&b, nir_iadd_imm(&b, base, 2),
                                    nir_imul_imm(&b, attempted, 6));
         t.store_word(&b, record, nir_imm_int(&b, (int)site.id));
         t.store_word(&b, nir_iadd_imm(&b, record, 1),
                      nir_imm_int(&b, (int)(site.kind * 8 + components)));
         for (unsigned i = 0; i < 4; i++) {
            nir_def* word = i < components ? payload[i] : nir_imm_int(&b, 0);
            t.store_word(&b, nir_iadd_imm(&b, record, 2 + i), word);
         }
      }
      nir_push_else(&b, room_if);
      {
         // capacity reached: never overwrite an old record
         nir_def* dropped = t.load_word(&b, nir_iadd_imm(&b, base, 1));
         t.store_word(&b, nir_iadd_imm(&b, base, 1), nir_iadd_imm(&b, dropped, 1));
      }
      nir_pop_if(&b, room_if);
   }
   nir_pop_if(&b, region_if);
}

// Insert logging at every catalogued site, in catalog (execution) order.
static void
instrument_sites(tracer& t)
{
   for (const trace_site& site : t.sites) {
      switch (site.kind) {
      case kGlslTraceAlu: {
         nir_alu_instr* alu = nir_instr_as_alu(site.instr);
         unsigned comps = site.components;
         emit_event(t, nir_after_instr(site.instr), site, comps,
                    [alu, comps](nir_builder* b, nir_def* words[4]) {
                       for (unsigned c = 0; c < comps; c++)
                          words[c] = nir_channel(b, &alu->def, c); // f32; printer emits floatBitsToUint
                    }, nullptr);
         break;
      }
      case kGlslTraceBranch: {
         nir_def* cond = site.nif->condition.ssa;
         emit_event(t, nir_before_cf_node(&site.nif->cf_node), site, 1,
                    [cond](nir_builder* b, nir_def* words[4]) {
                       words[0] = nir_b2i32(b, cond); // actual evaluation, 0 or 1
                    }, nullptr);
         break;
      }
      case kGlslTraceOutput: {
         nir_intrinsic_instr* in = nir_instr_as_intrinsic(site.instr);
         if (in->intrinsic == nir_intrinsic_copy_deref) {
            nir_deref_instr* src_d = nir_src_as_deref(in->src[1]);
            unsigned comps = site.components;
            emit_event(t, nir_after_instr(site.instr), site, comps,
                       [src_d, comps](nir_builder* b, nir_def* words[4]) {
                          nir_def* loaded = nir_load_deref(b, src_d);
                          for (unsigned c = 0; c < comps; c++)
                             words[c] = nir_channel(b, loaded, c);
                       }, nullptr);
         } else {
            nir_def* value = in->src[1].ssa;
            unsigned mask = site.write_mask;
            emit_event(t, nir_after_instr(site.instr), site, (unsigned)util_bitcount(mask),
                       [value, mask](nir_builder* b, nir_def* words[4]) {
                          // only the components this store actually writes, low to
                          // high; the unwritten parts of the output are never read
                          unsigned c = 0;
                          for (unsigned k = 0; k < value->num_components; k++) {
                             if (!(mask & (1u << k)))
                                continue;
                             words[c++] = nir_channel(b, value, k);
                          }
                       }, nullptr);
         }
         break;
      }
      case kGlslTraceDiscard: {
         nir_intrinsic_instr* in = nir_instr_as_intrinsic(site.instr);
         bool conditional = in->intrinsic == nir_intrinsic_terminate_if ||
                            in->intrinsic == nir_intrinsic_demote_if;
         // log before the discard, so records are observable even when it fires
         emit_event(t, nir_before_instr(site.instr), site, 0,
                    [](nir_builder*, nir_def*[4]) {},
                    conditional ? in->src[0].ssa : nullptr);
         break;
      }
      }
   }
}

static bool
outputs_are_float32(nir_shader* nir, const char** error, void* mem_ctx)
{
   nir_foreach_variable_with_modes(v, nir, nir_var_shader_out) {
      if (v->interface_type || glsl_type_is_array(v->type) ||
          !glsl_type_is_vector_or_scalar(glsl_without_array(v->type))) {
         *error = ralloc_strdup(mem_ctx,
                                "Tracing supports only float32 scalar and vector fragment outputs.");
         return false;
      }
      const glsl_type* bare = glsl_without_array(v->type);
      if (glsl_get_base_type(bare) != GLSL_TYPE_FLOAT || glsl_type_is_16bit(bare) ||
          glsl_contains_double(v->type)) {
         *error = ralloc_strdup(mem_ctx,
                                "Tracing supports only float32 scalar and vector fragment outputs.");
         return false;
      }
   }
   return true;
}

static bool
binding_is_free(nir_shader* nir, int binding, const char** error, void* mem_ctx)
{
   nir_foreach_variable_with_modes(v, nir, nir_var_mem_ssbo) {
      if (v->data.explicit_binding && (int)v->data.binding == binding) {
         *error = ralloc_asprintf(mem_ctx,
                                  "Trace SSBO binding %d is already used by an active shader storage block.",
                                  binding);
         return false;
      }
   }
   return true; // a UBO on the same binding is not a conflict
}

static void
collect_used_names(nir_shader* nir, std::set<std::string>& names)
{
   nir_foreach_variable_in_shader(v, nir) {
      if (v->name)
         names.insert(v->name);
      if (v->interface_type)
         names.insert(glsl_get_type_name(v->interface_type));
   }
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_function_temp_variable(v, impl) {
         if (v->name)
            names.insert(v->name);
      }
   }
}

static nir_variable*
create_trace_ssbo(tracer& t)
{
   std::set<std::string> used;
   collect_used_names(t.nir, used);

   // std430 runtime array of uint; explicit stride 0 selects the natural
   // 4-byte stride.
   const glsl_type* array_type = glsl_array_type(glsl_uint_type(), 0, 0);
   glsl_struct_field field = {};
   field.type = array_type;
   field.location = -1;
   field.offset = -1;
   field.name = "events";

   unsigned suffix = 0;
   do {
      t.type_name = suffix ? "glslopt_trace_block_" + std::to_string(suffix)
                           : "glslopt_trace_block";
      t.instance_name = suffix ? "glslopt_trace_log_" + std::to_string(suffix)
                               : "glslopt_trace_log";
      suffix++;
   } while (used.count(t.type_name) || used.count(t.instance_name));

   const glsl_type* iface = glsl_interface_type(&field, 1, GLSL_INTERFACE_PACKING_STD430, false,
                                                 t.type_name.c_str());
   nir_variable* var = nir_variable_create(t.nir, nir_var_mem_ssbo, iface, t.instance_name.c_str());
   var->interface_type = iface; // named block instance: var type is the interface
   var->data.explicit_binding = true;
   var->data.binding = (unsigned)t.config.ssbo_binding;
   return var;
}

static void
build_entry(tracer& t)
{
   // decl_reg intrinsics must remain in the start block, so the entry guard
   // cannot be inserted at the very top. Place it right after the last
   // decl_reg (these dominate the whole function); an empty start block is
   // fine to split.
   nir_cursor cursor = nir_before_impl(t.impl);
   nir_instr* last_decl = nullptr;
   nir_foreach_instr(instr, nir_start_block(t.impl)) {
      if (instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_decl_reg)
         last_decl = instr;
      else
         break;
   }
   if (last_decl)
      cursor = nir_after_instr(last_decl);

   nir_builder b = nir_builder_at(cursor);

   nir_def* coord = nir_load_frag_coord(&b);
   nir_def* fx = nir_ffloor(&b, nir_channel(&b, coord, 0));
   nir_def* fy = nir_ffloor(&b, nir_channel(&b, coord, 1));
   nir_def* ix = nir_iadd_imm(&b, nir_f2i32(&b, fx), -t.config.origin_x);
   nir_def* iy = nir_iadd_imm(&b, nir_f2i32(&b, fy), -t.config.origin_y);

   nir_def* in_x = nir_iand(&b, nir_ige_imm(&b, ix, 0), nir_ilt_imm(&b, ix, t.config.width));
   nir_def* in_y = nir_iand(&b, nir_ige_imm(&b, iy, 0), nir_ilt_imm(&b, iy, t.config.height));
   t.in_region = nir_iand(&b, in_x, in_y);

   nir_def* pixel = nir_iadd(&b, nir_imul_imm(&b, iy, t.config.width), ix);
   t.segment_base = nir_imul_imm(&b, pixel, (int64_t)t.segment_words);

   // Fragments inside the region start with empty counters; fragments
   // outside the region never touch the buffer and run the shader unchanged.
   nir_if* entry_if = nir_push_if(&b, t.in_region);
   {
      t.store_word(&b, t.segment_base, nir_imm_int(&b, 0));
      t.store_word(&b, nir_iadd_imm(&b, t.segment_base, 1), nir_imm_int(&b, 0));
   }
   nir_pop_if(&b, entry_if);
}

} // namespace

bool
glslopt_trace_fragment(nir_shader* nir, const glslopt_trace_config& config,
                       glslopt_trace_info* info, void* mem_ctx, const char** error)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT) {
      *error = ralloc_strdup(mem_ctx, "Tracing is only supported for fragment shaders.");
      return false;
   }
   if (config.width <= 0 || config.height <= 0) {
      *error = ralloc_strdup(mem_ctx, "Trace region width and height must be positive.");
      return false;
   }
   const uint64_t segment = 2ull + 6ull * (uint64_t)config.capacity;
   const uint64_t total_words = (uint64_t)config.width * (uint64_t)config.height * segment;
   if (total_words > (uint64_t)INT32_MAX) {
      *error = ralloc_strdup(mem_ctx,
                             "Trace buffer size exceeds the addressable 32-bit signed word range.");
      return false;
   }
   if (!outputs_are_float32(nir, error, mem_ctx))
      return false;
   if (!binding_is_free(nir, config.ssbo_binding, error, mem_ctx))
      return false;

   tracer t(nir, config, mem_ctx);
   t.segment_words = (unsigned)segment;

   nir_index_blocks(nir_shader_get_entrypoint(nir));

   // 1. catalog the original NIR
   catalog_cf(t, &nir_shader_get_entrypoint(nir)->body);

   // 2. create the trace SSBO and the per-pixel entry logic
   t.ssbo_var = create_trace_ssbo(t);
   build_entry(t);

   // 3. insert the logging code; instructions added here are never catalogued
   instrument_sites(t);

   // The instrumentation adds blocks, so all metadata is stale; refresh the
   // indices the printer and the catalog rely on.
   nir_progress(true, t.impl, nir_metadata_none);
   nir_index_blocks(t.impl);
   nir_index_ssa_defs(t.impl);
   nir_index_instrs(t.impl);
   nir_validate_shader(nir, "glsl-optimizer: after tracing");

   info->binding = config.ssbo_binding;
   info->segment_words = t.segment_words;
   info->site_count = (unsigned)t.sites.size();
   info->buffer_name = ralloc_strdup(mem_ctx, t.instance_name.c_str());
   info->buffer_type_name = ralloc_strdup(mem_ctx, t.type_name.c_str());

   glslopt_trace_site* out_sites = ralloc_array(mem_ctx, glslopt_trace_site, t.sites.size());
   for (size_t i = 0; i < t.sites.size(); i++) {
      const trace_site& s = t.sites[i];
      out_sites[i] = {s.id, s.kind, s.components, s.block,
                      s.output_name ? ralloc_strdup(mem_ctx, s.output_name) : nullptr,
                      s.write_mask};
   }
   info->sites = out_sites;
   return true;
}
