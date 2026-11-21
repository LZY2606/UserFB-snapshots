/**
 * Copyright (c) 2013 Mozilla Foundation and Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "node_internal.h"
#include "cue_internal.h"
#include "string_internal.h"

/**
 * @file cueselect.c
 *
 * Implementation of webvtt_cue_replace_text(), a structural edit of a cue's
 * node tree indexed by Unicode code-point offsets rather than UTF-8 byte
 * offsets. The edit is transactional: a brand new node tree and a brand new
 * normalized payload are built before anything in the cue is touched, so a
 * failed allocation or a rejected argument leaves the cue exactly as it was.
 */

typedef struct {
  webvtt_node **chain;
  webvtt_uint depth;
  char *text;
  webvtt_uint length;
  webvtt_uint count;
} text_segment;

typedef struct {
  text_segment *items;
  webvtt_uint length;
  webvtt_uint alloc;
} segment_list;

typedef struct {
  webvtt_node **chain;
  webvtt_uint depth;
  webvtt_uint pos;
  webvtt_timestamp value;
} stamp_rec;

typedef struct {
  webvtt_node **items;
  webvtt_uint depth;
  webvtt_uint alloc;
} chain_stack;

typedef struct {
  webvtt_node **chain;
  webvtt_uint depth;
  char *text;
  webvtt_uint length;
  webvtt_uint count;
  int owns_text;
} out_piece;

typedef struct {
  out_piece *items;
  webvtt_uint length;
  webvtt_uint alloc;
} piece_list;

typedef struct {
  segment_list segments;
  chain_stack chain;
  stamp_rec *stamps;
  webvtt_uint stamp_count;
  webvtt_uint stamp_alloc;
  webvtt_uint total;
} collect_ctx;

static webvtt_status
chain_push( chain_stack *stack, webvtt_node *node )
{
  webvtt_node **grown;
  webvtt_uint grown_alloc;

  if( stack->depth == stack->alloc ) {
    grown_alloc = stack->alloc == 0 ? 8 : stack->alloc * 2;
    grown = (webvtt_node **)webvtt_alloc( grown_alloc * sizeof( *grown ) );
    if( !grown ) {
      return WEBVTT_OUT_OF_MEMORY;
    }
    memcpy( grown, stack->items, stack->depth * sizeof( *grown ) );
    webvtt_free( stack->items );
    stack->items = grown;
    stack->alloc = grown_alloc;
  }
  stack->items[ stack->depth++ ] = node;
  return WEBVTT_SUCCESS;
}

static webvtt_status
dup_chain( webvtt_node ***out_chain, webvtt_uint *out_depth,
           const chain_stack *stack )
{
  webvtt_node **copy = 0;

  if( stack->depth > 0 ) {
    copy = (webvtt_node **)webvtt_alloc( stack->depth * sizeof( *copy ) );
    if( !copy ) {
      return WEBVTT_OUT_OF_MEMORY;
    }
    memcpy( copy, stack->items, stack->depth * sizeof( *copy ) );
  }
  *out_chain = copy;
  *out_depth = stack->depth;
  return WEBVTT_SUCCESS;
}

/**
 * Validate a length-bounded UTF-8 byte string and count its code points.
 * NUL and CR are rejected, LF is allowed, surrogates and overlong or
 * truncated sequences are rejected.
 */
static webvtt_status
validate_replacement( const char *text, webvtt_uint length,
                      webvtt_uint *codepoints )
{
  const unsigned char *p = (const unsigned char *)text;
  const unsigned char *end = p + length;
  webvtt_uint count = 0;

  while( p < end ) {
    webvtt_uint32 cp;
    int need;
    int i;
    unsigned char lead = *p;

    if( lead == 0 || lead == '\r' ) {
      return WEBVTT_INVALID_PARAM;
    }
    if( lead < 0x80 ) {
      cp = lead;
      need = 0;
    } else if( ( lead & 0xE0 ) == 0xC0 ) {
      cp = lead & 0x1F;
      need = 1;
      if( cp < 2 ) {
        return WEBVTT_INVALID_PARAM; /* overlong */
      }
    } else if( ( lead & 0xF0 ) == 0xE0 ) {
      cp = lead & 0x0F;
      need = 2;
    } else if( ( lead & 0xF8 ) == 0xF0 ) {
      cp = lead & 0x07;
      need = 3;
    } else {
      return WEBVTT_INVALID_PARAM;
    }
    ++p;

    for( i = 0; i < need; ++i ) {
      unsigned char trail;
      if( p >= end ) {
        return WEBVTT_INVALID_PARAM;
      }
      trail = *p;
      if( ( trail & 0xC0 ) != 0x80 ) {
        return WEBVTT_INVALID_PARAM;
      }
      cp = ( cp << 6 ) | ( trail & 0x3F );
      ++p;
    }

    if( ( cp >= 0xD800 && cp <= 0xDFFF ) || cp > 0x10FFFF ) {
      return WEBVTT_INVALID_PARAM;
    }
    ++count;
  }

  if( codepoints ) {
    *codepoints = count;
  }
  return WEBVTT_SUCCESS;
}

static int
is_supported_internal( webvtt_node_kind kind )
{
  switch( kind ) {
    case WEBVTT_HEAD_NODE:
    case WEBVTT_CLASS:
    case WEBVTT_ITALIC:
    case WEBVTT_BOLD:
    case WEBVTT_UNDERLINE:
    case WEBVTT_VOICE:
    case WEBVTT_LANG:
      return 1;
    default:
      return 0;
  }
}

static webvtt_status
collect_walk( collect_ctx *ctx, webvtt_node *node )
{
  webvtt_status status;
  webvtt_internal_node_data *nd;
  webvtt_uint i;

  if( WEBVTT_IS_LEAF( node->kind ) ) {
    if( node->kind == WEBVTT_TEXT ) {
      text_segment segment;
      webvtt_uint length = webvtt_string_length( &node->data.text );
      const char *text = webvtt_string_text( &node->data.text );

      memset( &segment, 0, sizeof( segment ) );
      if( length == 0 ) {
        return WEBVTT_SUCCESS;
      }
      segment.text = (char *)webvtt_alloc( length + 1 );
      if( !segment.text ) {
        return WEBVTT_OUT_OF_MEMORY;
      }
      memcpy( segment.text, text, length );
      segment.text[ length ] = 0;
      segment.length = length;
      segment.count = (webvtt_uint)webvtt_utf8_chcount( segment.text,
                                                        segment.text + length );
      status = dup_chain( &segment.chain, &segment.depth, &ctx->chain );
      if( WEBVTT_FAILED( status ) ) {
        webvtt_free( segment.text );
        return status;
      }

      if( ctx->segments.length == ctx->segments.alloc ) {
        webvtt_uint grown_alloc =
          ctx->segments.alloc == 0 ? 8 : ctx->segments.alloc * 2;
        text_segment *grown = (text_segment *)webvtt_alloc(
          grown_alloc * sizeof( *grown ) );
        if( !grown ) {
          webvtt_free( segment.chain );
          webvtt_free( segment.text );
          return WEBVTT_OUT_OF_MEMORY;
        }
        memcpy( grown, ctx->segments.items,
                ctx->segments.length * sizeof( *grown ) );
        webvtt_free( ctx->segments.items );
        ctx->segments.items = grown;
        ctx->segments.alloc = grown_alloc;
      }
      ctx->segments.items[ ctx->segments.length++ ] = segment;
      ctx->total += segment.count;
      return WEBVTT_SUCCESS;
    }

    if( node->kind == WEBVTT_TIME_STAMP ) {
      stamp_rec record;

      memset( &record, 0, sizeof( record ) );
      if( ctx->stamp_count == ctx->stamp_alloc ) {
        webvtt_uint grown_alloc =
          ctx->stamp_alloc == 0 ? 8 : ctx->stamp_alloc * 2;
        stamp_rec *grown = (stamp_rec *)webvtt_alloc(
          grown_alloc * sizeof( *grown ) );
        if( !grown ) {
          return WEBVTT_OUT_OF_MEMORY;
        }
        memcpy( grown, ctx->stamps,
                ctx->stamp_count * sizeof( *grown ) );
        webvtt_free( ctx->stamps );
        ctx->stamps = grown;
        ctx->stamp_alloc = grown_alloc;
      }

      record.pos = ctx->total;
      record.value = node->data.timestamp;
      status = dup_chain( &record.chain, &record.depth, &ctx->chain );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
      ctx->stamps[ ctx->stamp_count++ ] = record;
      return WEBVTT_SUCCESS;
    }

    return WEBVTT_NOT_SUPPORTED;
  }

  if( !is_supported_internal( node->kind ) ) {
    return WEBVTT_NOT_SUPPORTED;
  }

  nd = node->data.internal_data;
  if( !nd ) {
    return WEBVTT_NOT_SUPPORTED;
  }

  for( i = 0; i < nd->length; ++i ) {
    webvtt_node *child = nd->children[ i ];
    int pushed = 0;

    if( child->parent != node ) {
      return WEBVTT_INVALID_PARAM;
    }
    if( !WEBVTT_IS_LEAF( child->kind ) ) {
      if( WEBVTT_FAILED( status = chain_push( &ctx->chain, child ) ) ) {
        return status;
      }
      pushed = 1;
    }

    status = collect_walk( ctx, child );
    if( pushed ) {
      --ctx->chain.depth;
    }
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  return WEBVTT_SUCCESS;
}

static void
free_segments( segment_list *list )
{
  webvtt_uint i;
  for( i = 0; i < list->length; ++i ) {
    webvtt_free( list->items[ i ].chain );
    webvtt_free( list->items[ i ].text );
  }
  webvtt_free( list->items );
  list->items = 0;
  list->length = list->alloc = 0;
}

static void
free_stamps( stamp_rec *stamps, webvtt_uint count )
{
  webvtt_uint i;
  for( i = 0; i < count; ++i ) {
    webvtt_free( stamps[ i ].chain );
  }
  webvtt_free( stamps );
}

static int
same_chain( const out_piece *a, webvtt_node **b_chain, webvtt_uint b_depth )
{
  if( a->depth != b_depth ) {
    return 0;
  }
  if( b_depth == 0 ) {
    return 1;
  }
  return memcmp( a->chain, b_chain, b_depth * sizeof( webvtt_node * ) ) == 0;
}

/**
 * Append an output piece, coalescing it with the previous piece when both
 * share the exact same formatting chain so that surviving text of one
 * format is never split across sibling TEXT nodes.
 */
static webvtt_status
add_piece( piece_list *list, webvtt_node **chain, webvtt_uint depth,
           const char *text, webvtt_uint length )
{
  out_piece *piece;
  char *merged;

  if( length == 0 ) {
    return WEBVTT_SUCCESS;
  }

  if( list->length > 0 ) {
    piece = &list->items[ list->length - 1 ];
    if( same_chain( piece, chain, depth ) ) {
      merged = (char *)webvtt_alloc( piece->length + length );
      if( !merged ) {
        return WEBVTT_OUT_OF_MEMORY;
      }
      memcpy( merged, piece->text, piece->length );
      memcpy( merged + piece->length, text, length );
      if( piece->owns_text ) {
        webvtt_free( piece->text );
      }
      piece->text = merged;
      piece->length += length;
      piece->count += (webvtt_uint)webvtt_utf8_chcount( text,
                                                        text + length );
      piece->owns_text = 1;
      return WEBVTT_SUCCESS;
    }
  }

  if( list->length == list->alloc ) {
    webvtt_uint grown_alloc = list->alloc == 0 ? 8 : list->alloc * 2;
    out_piece *grown = (out_piece *)webvtt_alloc(
      grown_alloc * sizeof( *grown ) );
    if( !grown ) {
      return WEBVTT_OUT_OF_MEMORY;
    }
    memcpy( grown, list->items, list->length * sizeof( *grown ) );
    webvtt_free( list->items );
    list->items = grown;
    list->alloc = grown_alloc;
  }

  piece = &list->items[ list->length++ ];
  piece->chain = chain;
  piece->depth = depth;
  piece->text = (char *)text;
  piece->length = length;
  piece->count = (webvtt_uint)webvtt_utf8_chcount( text, text + length );
  piece->owns_text = 0;
  return WEBVTT_SUCCESS;
}

static void
free_pieces( piece_list *list )
{
  webvtt_uint i;
  for( i = 0; i < list->length; ++i ) {
    if( list->items[ i ].owns_text ) {
      webvtt_free( list->items[ i ].text );
    }
  }
  webvtt_free( list->items );
  list->items = 0;
  list->length = list->alloc = 0;
}

/**
 * Locate the segment and in-segment code-point offset of a boundary
 * position. A position equal to the total length resolves past the last
 * segment (segment index == list length).
 */
static void
boundary_location( const segment_list *segments, webvtt_uint pos,
                   webvtt_uint *seg_index, webvtt_uint *seg_offset )
{
  webvtt_uint cumulative = 0;
  webvtt_uint i;

  for( i = 0; i < segments->length; ++i ) {
    if( pos < cumulative + segments->items[ i ].count ) {
      *seg_index = i;
      *seg_offset = pos - cumulative;
      return;
    }
    cumulative += segments->items[ i ].count;
  }
  *seg_index = segments->length;
  *seg_offset = 0;
}

static const char *
codepoint_pointer( const char *text, webvtt_uint length, webvtt_uint offset )
{
  const char *p = text;
  const char *end = text + length;

  while( offset-- > 0 ) {
    webvtt_next_utf8( &p, end );
  }
  return p;
}

static void
chain_at( const segment_list *segments, webvtt_uint pos,
          webvtt_node ***chain, webvtt_uint *depth )
{
  webvtt_uint seg_index;
  webvtt_uint seg_offset;

  boundary_location( segments, pos, &seg_index, &seg_offset );
  if( seg_index < segments->length ) {
    *chain = segments->items[ seg_index ].chain;
    *depth = segments->items[ seg_index ].depth;
  } else if( segments->length > 0 ) {
    *chain = segments->items[ segments->length - 1 ].chain;
    *depth = segments->items[ segments->length - 1 ].depth;
  } else {
    *chain = 0;
    *depth = 0;
  }
}

/**
 * Build the linear list of surviving/replacement text pieces with the
 * formatting chain each piece must be attached to.
 */
static webvtt_status
build_pieces( const segment_list *segments, webvtt_uint begin,
              webvtt_uint end, const char *replacement,
              webvtt_uint replacement_bytes, piece_list *pieces )
{
  webvtt_status status;
  webvtt_uint begin_seg, begin_off, end_seg, end_off;
  webvtt_node **ins_chain;
  webvtt_uint ins_depth;
  webvtt_uint i;

  boundary_location( segments, begin, &begin_seg, &begin_off );
  boundary_location( segments, end, &end_seg, &end_off );
  chain_at( segments, begin, &ins_chain, &ins_depth );

  if( begin == end ) {
    for( i = 0; i < begin_seg; ++i ) {
      text_segment *segment = &segments->items[ i ];
      status = add_piece( pieces, segment->chain, segment->depth,
                          segment->text, segment->length );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    }
    if( begin_seg < segments->length ) {
      text_segment *segment = &segments->items[ begin_seg ];
      const char *split =
        codepoint_pointer( segment->text, segment->length, begin_off );
      status = add_piece( pieces, segment->chain, segment->depth,
                          segment->text,
                          (webvtt_uint)( split - segment->text ) );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
      status = add_piece( pieces, ins_chain, ins_depth,
                          replacement, replacement_bytes );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
      status = add_piece( pieces, segment->chain, segment->depth,
                          split,
                          segment->length - (webvtt_uint)( split -
                                                            segment->text ) );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    } else {
      status = add_piece( pieces, ins_chain, ins_depth,
                          replacement, replacement_bytes );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    }
    for( i = begin_seg + 1; i < segments->length; ++i ) {
      text_segment *segment = &segments->items[ i ];
      status = add_piece( pieces, segment->chain, segment->depth,
                          segment->text, segment->length );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    }
    return WEBVTT_SUCCESS;
  }

  /* begin < end: replacement of a non-empty range */
  for( i = 0; i < begin_seg; ++i ) {
    text_segment *segment = &segments->items[ i ];
    status = add_piece( pieces, segment->chain, segment->depth,
                        segment->text, segment->length );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  if( begin_seg < segments->length ) {
    text_segment *segment = &segments->items[ begin_seg ];
    const char *split =
      codepoint_pointer( segment->text, segment->length, begin_off );
    status = add_piece( pieces, segment->chain, segment->depth,
                        segment->text, (webvtt_uint)( split - segment->text ) );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  status = add_piece( pieces, ins_chain, ins_depth,
                      replacement, replacement_bytes );
  if( WEBVTT_FAILED( status ) ) {
    return status;
  }

  if( end_seg < segments->length ) {
    text_segment *segment = &segments->items[ end_seg ];
    const char *split =
      codepoint_pointer( segment->text, segment->length, end_off );
    status = add_piece( pieces, segment->chain, segment->depth,
                        split,
                        segment->length - (webvtt_uint)( split -
                                                          segment->text ) );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  for( i = end_seg + 1; i < segments->length; ++i ) {
    text_segment *segment = &segments->items[ i ];
    status = add_piece( pieces, segment->chain, segment->depth,
                        segment->text, segment->length );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  return WEBVTT_SUCCESS;
}

typedef struct {
  stamp_rec *items;
  webvtt_uint count;
} out_stamps;

/**
 * Keep boundary anchors, drop strictly interior ones and shift the anchors
 * following the edit onto their new code-point positions. Chain ownership
 * moves out of the source array: transferred chains belong to the output
 * and dropped chains are freed here, so every source chain is nulled.
 */
static webvtt_status
build_stamps( stamp_rec *stamps, webvtt_uint stamp_count,
              webvtt_uint begin, webvtt_uint end, webvtt_uint removed,
              webvtt_uint inserted, out_stamps *out )
{
  webvtt_uint i;

  out->items = (stamp_rec *)webvtt_alloc(
    ( stamp_count ? stamp_count : 1 ) * sizeof( *out->items ) );
  if( !out->items ) {
    return WEBVTT_OUT_OF_MEMORY;
  }
  out->count = 0;

  for( i = 0; i < stamp_count; ++i ) {
    if( begin < end && stamps[ i ].pos > begin &&
        stamps[ i ].pos < end ) {
      webvtt_free( stamps[ i ].chain );
      stamps[ i ].chain = 0;
      continue;
    }
    /* An anchor at begin always stays before the inserted text. */
    if( stamps[ i ].pos > begin ) {
      stamps[ i ].pos = stamps[ i ].pos - removed + inserted;
    }
    out->items[ out->count++ ] = stamps[ i ];
    stamps[ i ].chain = 0; /* ownership transferred to out */
  }
  return WEBVTT_SUCCESS;
}

static webvtt_status
open_like_node( webvtt_node *parent, webvtt_node *tmpl, webvtt_node **out )
{
  webvtt_internal_node_data *data = tmpl->data.internal_data;
  webvtt_status status;

  if( tmpl->kind == WEBVTT_LANG ) {
    return webvtt_create_lang_node( out, parent, data->css_classes,
                                    &data->lang );
  }

  status = webvtt_create_internal_node( out, parent, tmpl->kind,
                                        data->css_classes,
                                        &data->annotation );
  if( WEBVTT_FAILED( status ) ) {
    return status;
  }

  /* The parser propagates the active <lang> onto every nested internal
   * node. Replicate it so the normalized payload reparses into a tree
   * identical to the one built here. */
  if( webvtt_string_length( &data->lang ) > 0 ) {
    webvtt_release_string( &( *out )->data.internal_data->lang );
    webvtt_copy_string( &( *out )->data.internal_data->lang,
                        &data->lang );
  }
  return WEBVTT_SUCCESS;
}

typedef struct {
  webvtt_node **new_nodes;
  webvtt_node **templates;
  webvtt_uint depth;
  webvtt_uint alloc;
} open_chain;

static webvtt_status
switch_chain( open_chain *open, webvtt_node *head,
              webvtt_node **chain, webvtt_uint target_depth )
{
  webvtt_status status;

  /* Close nodes up to the common prefix of the open and target chains.
   * Comparison is against the source template node recorded when the new
   * node was opened, since chain[] references the old tree. */
  while( open->depth > 0 ) {
    if( open->depth <= target_depth &&
        open->templates[ open->depth - 1 ] == chain[ open->depth - 1 ] ) {
      break;
    }
    --open->depth;
  }

  /* Open the formatting nodes the target chain still needs. */
  while( open->depth < target_depth ) {
    webvtt_node *created = 0;
    webvtt_node *parent =
      open->depth == 0 ? head : open->new_nodes[ open->depth - 1 ];

    status = open_like_node( parent, chain[ open->depth ], &created );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
    status = webvtt_attach_node( parent, created );
    if( WEBVTT_FAILED( status ) ) {
      webvtt_release_node( &created );
      return status;
    }
    webvtt_release_node( &created );

    if( open->depth == open->alloc ) {
      webvtt_uint grown_alloc = open->alloc == 0 ? 8 : open->alloc * 2;
      webvtt_node **grown_new = (webvtt_node **)webvtt_alloc(
        grown_alloc * sizeof( *grown_new ) );
      webvtt_node **grown_tmpl = (webvtt_node **)webvtt_alloc(
        grown_alloc * sizeof( *grown_tmpl ) );
      if( !grown_new || !grown_tmpl ) {
        webvtt_free( grown_new );
        webvtt_free( grown_tmpl );
        return WEBVTT_OUT_OF_MEMORY;
      }
      memcpy( grown_new, open->new_nodes,
              open->depth * sizeof( *grown_new ) );
      memcpy( grown_tmpl, open->templates,
              open->depth * sizeof( *grown_tmpl ) );
      webvtt_free( open->new_nodes );
      webvtt_free( open->templates );
      open->new_nodes = grown_new;
      open->templates = grown_tmpl;
      open->alloc = grown_alloc;
    }

    open->templates[ open->depth ] = chain[ open->depth ];
    open->new_nodes[ open->depth ] =
      parent->data.internal_data->children[
        parent->data.internal_data->length - 1 ];
    ++open->depth;
  }
  return WEBVTT_SUCCESS;
}

static webvtt_node *
chain_current( const open_chain *open, webvtt_node *head )
{
  return open->depth == 0 ? head : open->new_nodes[ open->depth - 1 ];
}

static webvtt_status
emit_text_node( webvtt_node *parent, const char *text, webvtt_uint length )
{
  webvtt_string str;
  webvtt_node *node = 0;
  webvtt_status status;

  webvtt_init_string( &str );
  status = webvtt_string_append( &str, text, length );
  if( WEBVTT_FAILED( status ) ) {
    webvtt_release_string( &str );
    return status;
  }
  status = webvtt_create_text_node( &node, parent, &str );
  webvtt_release_string( &str );
  if( WEBVTT_FAILED( status ) ) {
    return status;
  }
  status = webvtt_attach_node( parent, node );
  webvtt_release_node( &node );
  return status;
}

static webvtt_status
build_tree( webvtt_node *head, const piece_list *pieces,
            const out_stamps *stamps )
{
  webvtt_status status = WEBVTT_SUCCESS;
  open_chain open;
  webvtt_uint text_pos = 0;   /* code points emitted so far */
  webvtt_uint pi = 0;
  webvtt_uint piece_cp = 0;   /* code points emitted from current piece */
  webvtt_uint si = 0;

  memset( &open, 0, sizeof( open ) );

  while( pi < pieces->length || si < stamps->count ) {
    int emit_stamp = pi >= pieces->length ||
                     ( si < stamps->count &&
                       stamps->items[ si ].pos <= text_pos );

    if( emit_stamp ) {
      stamp_rec *stamp = &stamps->items[ si ];
      webvtt_node *node = 0;
      webvtt_node *parent;

      status = switch_chain( &open, head, stamp->chain, stamp->depth );
      if( WEBVTT_FAILED( status ) ) {
        goto done;
      }
      parent = chain_current( &open, head );
      status = webvtt_create_timestamp_node( &node, parent, stamp->value );
      if( WEBVTT_FAILED( status ) ) {
        goto done;
      }
      status = webvtt_attach_node( parent, node );
      webvtt_release_node( &node );
      if( WEBVTT_FAILED( status ) ) {
        goto done;
      }
      ++si;
    } else {
      out_piece *piece = &pieces->items[ pi ];
      webvtt_uint remaining = piece->count - piece_cp;
      const char *start = codepoint_pointer( piece->text, piece->length,
                                             piece_cp );
      const char *stop = piece->text + piece->length;
      webvtt_uint emit_count = remaining;
      webvtt_node *parent;

      status = switch_chain( &open, head, piece->chain, piece->depth );
      if( WEBVTT_FAILED( status ) ) {
        goto done;
      }
      parent = chain_current( &open, head );

      /* A surviving anchor inside this piece splits the text node. */
      if( si < stamps->count &&
          stamps->items[ si ].pos < text_pos + remaining ) {
        webvtt_uint offset = stamps->items[ si ].pos - text_pos;
        stop = codepoint_pointer( start,
                                  (webvtt_uint)( ( piece->text +
                                                   piece->length ) - start ),
                                  offset );
        emit_count = offset;
      }

      status = emit_text_node( parent, start,
                               (webvtt_uint)( stop - start ) );
      if( WEBVTT_FAILED( status ) ) {
        goto done;
      }
      piece_cp += emit_count;
      text_pos += emit_count;
      if( piece_cp >= piece->count ) {
        ++pi;
        piece_cp = 0;
      }
    }
  }

done:
  webvtt_free( open.new_nodes );
  webvtt_free( open.templates );
  return status;
}

static const char *
tag_name( webvtt_node_kind kind )
{
  switch( kind ) {
    case WEBVTT_CLASS:
      return "c";
    case WEBVTT_ITALIC:
      return "i";
    case WEBVTT_BOLD:
      return "b";
    case WEBVTT_UNDERLINE:
      return "u";
    case WEBVTT_VOICE:
      return "v";
    case WEBVTT_LANG:
      return "lang";
    default:
      return "";
  }
}

static webvtt_status
append_char( webvtt_string *out, char ch )
{
  /* webvtt_string_append() routes allocation failures through grow(), which
   * returns WEBVTT_OUT_OF_MEMORY cleanly, unlike putc()/detach(). */
  return webvtt_string_append( out, &ch, 1 );
}

static webvtt_status
append_escaped_text( webvtt_string *out, const char *text,
                     webvtt_uint length )
{
  webvtt_status status;
  webvtt_uint i;

  for( i = 0; i < length; ++i ) {
    char ch = text[ i ];
    if( ch == '&' ) {
      status = webvtt_string_append( out, "&amp;", 5 );
    } else if( ch == '<' ) {
      status = webvtt_string_append( out, "&lt;", 4 );
    } else if( ch == '>' ) {
      status = webvtt_string_append( out, "&gt;", 4 );
    } else {
      status = append_char( out, ch );
    }
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }
  return WEBVTT_SUCCESS;
}

static webvtt_status
append_timestamp_text( webvtt_string *out, webvtt_timestamp timestamp )
{
  char buffer[ 32 ];
  int written;
  webvtt_uint hours = (webvtt_uint)( timestamp / 3600000 );
  webvtt_uint minutes = (webvtt_uint)( ( timestamp % 3600000 ) / 60000 );
  webvtt_uint seconds = (webvtt_uint)( ( timestamp % 60000 ) / 1000 );
  webvtt_uint millis = (webvtt_uint)( timestamp % 1000 );

  written = snprintf( buffer, sizeof( buffer ),
                      "<%02u:%02u:%02u.%03u>", hours, minutes,
                      seconds, millis );
  return webvtt_string_append( out, buffer, written );
}

static webvtt_status
serialize_node( webvtt_node *node, webvtt_string *out )
{
  webvtt_status status;
  webvtt_internal_node_data *data;
  webvtt_uint i;

  if( node->kind == WEBVTT_TEXT ) {
    return append_escaped_text( out, webvtt_string_text( &node->data.text ),
                                webvtt_string_length( &node->data.text ) );
  }
  if( node->kind == WEBVTT_TIME_STAMP ) {
    return append_timestamp_text( out, node->data.timestamp );
  }

  data = node->data.internal_data;

  if( node->kind != WEBVTT_HEAD_NODE ) {
    const char *name = tag_name( node->kind );

    status = append_char( out, '<' );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
    status = webvtt_string_append( out, name, (int)strlen( name ) );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
    if( data->css_classes ) {
      for( i = 0; i < data->css_classes->length; ++i ) {
        status = append_char( out, '.' );
        if( WEBVTT_FAILED( status ) ) {
          return status;
        }
        status = webvtt_string_append_string(
          out, &data->css_classes->items[ i ] );
        if( WEBVTT_FAILED( status ) ) {
          return status;
        }
      }
    }
    if( node->kind == WEBVTT_VOICE &&
        webvtt_string_length( &data->annotation ) > 0 ) {
      status = append_char( out, ' ' );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
      status = webvtt_string_append_string( out, &data->annotation );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    }
    if( node->kind == WEBVTT_LANG &&
        webvtt_string_length( &data->lang ) > 0 ) {
      status = append_char( out, ' ' );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
      status = webvtt_string_append_string( out, &data->lang );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    }
    status = append_char( out, '>' );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  for( i = 0; i < data->length; ++i ) {
    status = serialize_node( data->children[ i ], out );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  if( node->kind != WEBVTT_HEAD_NODE ) {
    status = webvtt_string_append( out, "</", 2 );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
    status = webvtt_string_append( out, tag_name( node->kind ),
                                   (int)strlen( tag_name( node->kind ) ) );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
    status = webvtt_string_putc( out, '>' );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  return WEBVTT_SUCCESS;
}

WEBVTT_EXPORT webvtt_status
webvtt_cue_replace_text( webvtt_cue *cue, webvtt_uint begin, webvtt_uint end,
                         const char *replacement, webvtt_uint length )
{
  webvtt_status status;
  collect_ctx ctx;
  piece_list pieces;
  out_stamps stamps;
  webvtt_node *new_head = 0;
  webvtt_string new_body;
  webvtt_uint inserted = 0;
  webvtt_uint i;

  memset( &ctx, 0, sizeof( ctx ) );
  memset( &pieces, 0, sizeof( pieces ) );
  memset( &stamps, 0, sizeof( stamps ) );
  webvtt_init_string( &new_body );

  if( !cue ) {
    return WEBVTT_INVALID_PARAM;
  }
  if( length > 0 && !replacement ) {
    return WEBVTT_INVALID_PARAM;
  }

  status = validate_replacement( replacement, length, &inserted );
  if( WEBVTT_FAILED( status ) ) {
    return status;
  }

  if( cue->node_head ) {
    status = collect_walk( &ctx, cue->node_head );
    if( WEBVTT_FAILED( status ) ) {
      goto fail;
    }
  }

  if( end > ctx.total || begin > end ) {
    status = WEBVTT_INVALID_PARAM;
    goto fail;
  }

  status = build_pieces( &ctx.segments, begin, end, replacement, length,
                         &pieces );
  if( WEBVTT_FAILED( status ) ) {
    goto fail;
  }

  status = build_stamps( ctx.stamps, ctx.stamp_count, begin, end,
                         end - begin, inserted, &stamps );
  if( WEBVTT_FAILED( status ) ) {
    goto fail;
  }

  status = webvtt_create_head_node( &new_head );
  if( WEBVTT_FAILED( status ) ) {
    goto fail;
  }
  status = build_tree( new_head, &pieces, &stamps );
  if( WEBVTT_FAILED( status ) ) {
    goto fail;
  }
  status = serialize_node( new_head, &new_body );
  if( WEBVTT_FAILED( status ) ) {
    goto fail;
  }

  /* Commit: swap in the new tree and payload, dropping the cue's reference
   * to the old ones. Trees and payloads retained elsewhere survive. */
  {
    webvtt_node *old_head = cue->node_head;
    webvtt_string old_body = cue->body; /* take over the old reference */

    webvtt_copy_string( &cue->body, &new_body );
    webvtt_release_string( &old_body );

    cue->node_head = new_head;
    new_head = 0;
    webvtt_release_node( &old_head );
  }

  status = WEBVTT_SUCCESS;
  goto done;

fail:
  webvtt_release_node( &new_head );

done:
  webvtt_release_string( &new_body );
  for( i = 0; i < stamps.count; ++i ) {
    webvtt_free( stamps.items[ i ].chain );
  }
  webvtt_free( stamps.items );
  free_pieces( &pieces );
  free_stamps( ctx.stamps, ctx.stamp_count );
  free_segments( &ctx.segments );
  webvtt_free( ctx.chain.items );
  return status;
}
