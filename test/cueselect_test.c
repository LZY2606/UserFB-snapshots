/*
 * Regression tests for webvtt_cue_replace_text().
 *
 * Registered as three groups - selection, anchors and transaction - and
 * driven by task_support/driver.c through task_support/check.h. There is
 * deliberately no main() in this file.
 */

#include <webvtt/parser.h>
#include <webvtt/cue.h>
#include <webvtt/node.h>
#include "task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* parsing helpers                                                     */
/* ------------------------------------------------------------------ */

struct cue_capture {
  webvtt_cue *cue;
  unsigned errors;
};

static void
capture_cue( void *userdata, webvtt_cue *cue )
{
  struct cue_capture *capture = (struct cue_capture *)userdata;
  capture->cue = cue;
  webvtt_ref_cue( cue );
  /* The parser hands over one reference; return it so that the only live
   * reference is the one this callback just took. */
  webvtt_release_cue( &cue );
}

static int
count_error( void *userdata, webvtt_uint line, webvtt_uint col,
             webvtt_error error )
{
  struct cue_capture *capture = (struct cue_capture *)userdata;
  (void)line;
  (void)col;
  (void)error;
  ++capture->errors;
  return 0;
}

static webvtt_cue *
parse_vtt( const char *vtt )
{
  struct cue_capture capture;
  webvtt_parser parser = 0;
  size_t i;

  memset( &capture, 0, sizeof( capture ) );
  if( webvtt_create_parser( capture_cue, count_error, &capture,
                            &parser ) != WEBVTT_SUCCESS ) {
    return 0;
  }
  for( i = 0; i < strlen( vtt ); ++i ) {
    if( WEBVTT_FAILED( webvtt_parse_chunk( parser, vtt + i, 1 ) ) ) {
      webvtt_delete_parser( parser );
      return 0;
    }
  }
  if( WEBVTT_FAILED( webvtt_finish_parsing( parser ) ) ) {
    webvtt_delete_parser( parser );
    return 0;
  }
  webvtt_delete_parser( parser );
  if( capture.errors || !capture.cue ) {
    webvtt_release_cue( &capture.cue );
    return 0;
  }
  return capture.cue;
}

static webvtt_cue *
parse_body( const char *body )
{
  char vtt[ 1024 ];

  snprintf( vtt, sizeof( vtt ),
            "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\n%s\n\n", body );
  return parse_vtt( vtt );
}

/* ------------------------------------------------------------------ */
/* tree inspection helpers                                             */
/* ------------------------------------------------------------------ */

static void
collect_text( webvtt_node *node, char *buffer, size_t cap )
{
  size_t used = strlen( buffer );

  if( !node ) {
    return;
  }
  if( node->kind == WEBVTT_TEXT ) {
    const char *text = webvtt_string_text( &node->data.text );
    size_t len = webvtt_string_length( &node->data.text );
    if( used + len < cap ) {
      memcpy( buffer + used, text, len );
      buffer[ used + len ] = 0;
    }
  } else if( WEBVTT_IS_VALID_INTERNAL_NODE( node->kind ) ) {
    webvtt_internal_node_data *data = node->data.internal_data;
    webvtt_uint i;
    for( i = 0; i < data->length; ++i ) {
      collect_text( data->children[ i ], buffer, cap );
    }
  }
}

static unsigned
count_kind( webvtt_node *node, webvtt_node_kind kind )
{
  unsigned total = 0;
  webvtt_uint i;

  if( !node ) {
    return 0;
  }
  if( node->kind == kind ) {
    ++total;
  }
  if( WEBVTT_IS_VALID_INTERNAL_NODE( node->kind ) ) {
    webvtt_internal_node_data *data = node->data.internal_data;
    for( i = 0; i < data->length; ++i ) {
      total += count_kind( data->children[ i ], kind );
    }
  }
  return total;
}

static webvtt_node *
find_kind( webvtt_node *node, webvtt_node_kind kind )
{
  webvtt_uint i;

  if( !node ) {
    return 0;
  }
  if( node->kind == kind ) {
    return node;
  }
  if( WEBVTT_IS_VALID_INTERNAL_NODE( node->kind ) ) {
    webvtt_internal_node_data *data = node->data.internal_data;
    for( i = 0; i < data->length; ++i ) {
      webvtt_node *found = find_kind( data->children[ i ], kind );
      if( found ) {
        return found;
      }
    }
  }
  return 0;
}

static int
parents_valid( webvtt_node *node, webvtt_node *parent )
{
  webvtt_uint i;

  if( !node ) {
    return 1;
  }
  if( node->parent != parent ) {
    return 0;
  }
  if( WEBVTT_IS_VALID_INTERNAL_NODE( node->kind ) ) {
    webvtt_internal_node_data *data = node->data.internal_data;
    for( i = 0; i < data->length; ++i ) {
      if( !parents_valid( data->children[ i ], node ) ) {
        return 0;
      }
    }
  }
  return 1;
}

struct stamp_walk {
  webvtt_timestamp values[ 16 ];
  unsigned positions[ 16 ];
  unsigned count;
  unsigned codepoints;
};

static void
walk_stamps( webvtt_node *node, struct stamp_walk *walk )
{
  if( node->kind == WEBVTT_TEXT ) {
    walk->codepoints += (unsigned)webvtt_utf8_chcount(
      webvtt_string_text( &node->data.text ),
      webvtt_string_text( &node->data.text ) +
        webvtt_string_length( &node->data.text ) );
  } else if( node->kind == WEBVTT_TIME_STAMP ) {
    if( walk->count < 16 ) {
      walk->values[ walk->count ] = node->data.timestamp;
      walk->positions[ walk->count ] = walk->codepoints;
      ++walk->count;
    }
  } else if( WEBVTT_IS_VALID_INTERNAL_NODE( node->kind ) ) {
    webvtt_internal_node_data *data = node->data.internal_data;
    webvtt_uint i;
    for( i = 0; i < data->length; ++i ) {
      walk_stamps( data->children[ i ], walk );
    }
  }
}

static char
kind_letter( webvtt_node_kind kind )
{
  switch( kind ) {
    case WEBVTT_HEAD_NODE: return 'H';
    case WEBVTT_CLASS: return 'c';
    case WEBVTT_ITALIC: return 'i';
    case WEBVTT_BOLD: return 'b';
    case WEBVTT_UNDERLINE: return 'u';
    case WEBVTT_VOICE: return 'v';
    case WEBVTT_LANG: return 'l';
    default: return '?';
  }
}

static void
describe_append( char *buffer, size_t cap, size_t *used, const char *text )
{
  size_t len = strlen( text );
  if( *used + len < cap ) {
    memcpy( buffer + *used, text, len + 1 );
    *used += len;
  }
}

static void
describe_node( webvtt_node *node, char *buffer, size_t cap, size_t *used )
{
  char number[ 32 ];

  if( node->kind == WEBVTT_TEXT ) {
    describe_append( buffer, cap, used, "'" );
    describe_append( buffer, cap, used,
                     webvtt_string_text( &node->data.text ) );
    describe_append( buffer, cap, used, "'" );
    return;
  }
  if( node->kind == WEBVTT_TIME_STAMP ) {
    snprintf( number, sizeof( number ), "#%llu",
              (unsigned long long)node->data.timestamp );
    describe_append( buffer, cap, used, number );
    return;
  }

  {
    webvtt_internal_node_data *data = node->data.internal_data;
    webvtt_uint i;

    number[ 0 ] = kind_letter( node->kind );
    number[ 1 ] = 0;
    describe_append( buffer, cap, used, number );

    if( node->kind != WEBVTT_HEAD_NODE ) {
      if( data->css_classes ) {
        for( i = 0; i < data->css_classes->length; ++i ) {
          describe_append( buffer, cap, used, "." );
          describe_append( buffer, cap, used,
                           webvtt_string_text(
                             &data->css_classes->items[ i ] ) );
        }
      }
      if( webvtt_string_length( &data->annotation ) > 0 ) {
        describe_append( buffer, cap, used, "(" );
        describe_append( buffer, cap, used,
                         webvtt_string_text( &data->annotation ) );
        describe_append( buffer, cap, used, ")" );
      }
      if( webvtt_string_length( &data->lang ) > 0 ) {
        describe_append( buffer, cap, used, "{" );
        describe_append( buffer, cap, used,
                         webvtt_string_text( &data->lang ) );
        describe_append( buffer, cap, used, "}" );
      }
    }

    describe_append( buffer, cap, used, "(" );
    for( i = 0; i < data->length; ++i ) {
      describe_node( data->children[ i ], buffer, cap, used );
    }
    describe_append( buffer, cap, used, ")" );
  }
}

static void
describe_tree( webvtt_node *head, char *buffer, size_t cap )
{
  size_t used = 0;
  buffer[ 0 ] = 0;
  describe_node( head, buffer, cap, &used );
}

/* ------------------------------------------------------------------ */
/* selection group                                                     */
/* ------------------------------------------------------------------ */

static int
selection_entities_cjk_emoji( void )
{
  webvtt_cue *cue = parse_body(
    "<v Alice><c.red>A&amp;<i>中\xF0\x9F\x98\x80</i></c>B</v>" );
  char visible[ 256 ];
  char body[ 512 ];
  webvtt_node *voice;
  webvtt_node *cls;
  webvtt_internal_node_data *vdata;
  webvtt_internal_node_data *cdata;

  TASK_CHECK( cue != 0 );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "A&中\xF0\x9F\x98\x80" "B" ) == 0 );

  /* Replace "&中😀" - positions 1 through 4 - with literal markup text. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 4, "X<Y&Z", 5 ) ==
              WEBVTT_SUCCESS );

  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "AX<Y&ZB" ) == 0 );

  snprintf( body, sizeof( body ), "%s",
            webvtt_string_text( &cue->body ) );
  TASK_CHECK( strstr( body, "&lt;" ) != 0 );
  TASK_CHECK( strstr( body, "&amp;" ) != 0 );
  TASK_CHECK( strstr( body, "<Y" ) == 0 );
  TASK_CHECK( strstr( body, "<i>" ) == 0 ); /* emptied italic was pruned */

  voice = find_kind( cue->node_head, WEBVTT_VOICE );
  TASK_CHECK( voice != 0 );
  TASK_CHECK( strcmp( webvtt_string_text(
              &voice->data.internal_data->annotation ), "Alice" ) == 0 );
  vdata = voice->data.internal_data;
  TASK_CHECK( vdata->length == 2 );
  TASK_CHECK( vdata->children[ 0 ]->kind == WEBVTT_CLASS );
  TASK_CHECK( vdata->children[ 1 ]->kind == WEBVTT_TEXT );
  TASK_CHECK( strcmp( webvtt_string_text(
              &vdata->children[ 1 ]->data.text ), "B" ) == 0 );

  cls = vdata->children[ 0 ];
  cdata = cls->data.internal_data;
  TASK_CHECK( cdata->css_classes->length == 1 );
  TASK_CHECK( strcmp( webvtt_string_text(
              &cdata->css_classes->items[ 0 ] ), "red" ) == 0 );
  /* The surviving "A" and the inserted text share one format chain and
   * therefore normalize into a single TEXT node. */
  TASK_CHECK( cdata->length == 1 );
  TASK_CHECK( cdata->children[ 0 ]->kind == WEBVTT_TEXT );
  TASK_CHECK( strcmp( webvtt_string_text(
              &cdata->children[ 0 ]->data.text ), "AX<Y&Z" ) == 0 );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_ITALIC ) == 0 );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );

  webvtt_release_cue( &cue );
  return 0;
}

static int
selection_cross_nested_branches( void )
{
  webvtt_cue *cue = parse_body( "ab<b>cd<i>ef</i>gh</b>ij" );
  webvtt_node *bold;
  webvtt_internal_node_data *bdata;
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );

  /* Delete every bold character: the emptied <b> branch must be pruned and
   * the two surviving root runs must not inherit any formatting. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 2, 8, "", 0 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "abij" ) == 0 );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_BOLD ) == 0 );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_ITALIC ) == 0 );
  TASK_CHECK( cue->node_head->data.internal_data->length == 1 );
  TASK_CHECK( cue->node_head->data.internal_data->children[ 0 ]->kind ==
              WEBVTT_TEXT );
  TASK_CHECK( strcmp( webvtt_string_text(
    &cue->node_head->data.internal_data->children[ 0 ]->data.text ),
    "abij" ) == 0 );
  webvtt_release_cue( &cue );

  /* Survivors on both sides of the selection keep their distinct formats;
   * same-format survivors coalesce instead of being split. */
  cue = parse_body( "ab<b>cd<i>ef</i>gh</b>ij" );
  TASK_CHECK( cue != 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 3, 7, "Q", 1 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "abcQhij" ) == 0 );

  bold = find_kind( cue->node_head, WEBVTT_BOLD );
  TASK_CHECK( bold != 0 );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_ITALIC ) == 0 );
  bdata = bold->data.internal_data;
  TASK_CHECK( bdata->length == 1 );
  TASK_CHECK( bdata->children[ 0 ]->kind == WEBVTT_TEXT );
  TASK_CHECK( strcmp( webvtt_string_text(
              &bdata->children[ 0 ]->data.text ), "cQh" ) == 0 );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );

  webvtt_release_cue( &cue );
  return 0;
}

static int
selection_head_and_tail_insert( void )
{
  webvtt_cue *cue = parse_body( "<b>xy</b>" );
  char visible[ 256 ];
  webvtt_node *bold;

  TASK_CHECK( cue != 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 0, "[", 1 ) ==
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 3, 3, "]", 1 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "[xy]" ) == 0 );

  bold = find_kind( cue->node_head, WEBVTT_BOLD );
  TASK_CHECK( bold != 0 );
  TASK_CHECK( bold->data.internal_data->length == 1 );
  TASK_CHECK( strcmp( webvtt_string_text(
              &bold->data.internal_data->children[ 0 ]->data.text ),
              "[xy]" ) == 0 );
  webvtt_release_cue( &cue );

  /* An empty cue has no formatting chain: inserted text hangs off root. */
  TASK_CHECK( webvtt_create_cue( &cue ) == WEBVTT_SUCCESS );
  TASK_CHECK( cue->node_head == 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 0, "z", 1 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "z" ) == 0 );
  TASK_CHECK( cue->node_head->data.internal_data->length == 1 );
  TASK_CHECK( cue->node_head->data.internal_data->children[ 0 ]->kind ==
              WEBVTT_TEXT );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );
  webvtt_release_cue( &cue );
  return 0;
}

static int
selection_clear_and_rebuild( void )
{
  webvtt_cue *cue = parse_body( "ab<b>cd</b>ef" );
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );

  /* Remove every code point: the tree collapses to an empty head node and
   * every emptied formatting branch is pruned. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 6, "", 0 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( visible[ 0 ] == 0 );
  TASK_CHECK( cue->node_head->kind == WEBVTT_HEAD_NODE );
  TASK_CHECK( cue->node_head->data.internal_data->length == 0 );
  TASK_CHECK( strcmp( webvtt_string_text( &cue->body ), "" ) == 0 );

  /* Editing again rebuilds from the empty head (root chain). */
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 0, "gh", 2 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "gh" ) == 0 );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_BOLD ) == 0 );
  TASK_CHECK( cue->node_head->data.internal_data->length == 1 );
  TASK_CHECK( cue->node_head->data.internal_data->children[ 0 ]->kind ==
              WEBVTT_TEXT );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );
  webvtt_release_cue( &cue );
  return 0;
}

static int
selection_deep_nesting_roundtrip( void )
{
  webvtt_cue *cue;
  webvtt_cue *reread;
  webvtt_node *lang;
  webvtt_node *voice;
  webvtt_node *underline;
  webvtt_node *cls;
  webvtt_internal_node_data *ldata;
  char before_desc[ 512 ];
  char after_desc[ 512 ];
  char visible[ 256 ];

  /* voice -> class (two classes) -> underline -> lang -> bold -> text */
  cue = parse_body(
    "<v Bob><c.a.b><u><lang ja><b>wxyz</b></lang></u></c></v>" );
  TASK_CHECK( cue != 0 );

  /* Replace the middle two code points "xy", inheriting the full chain. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 3, "Q", 1 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "wQz" ) == 0 );

  voice = find_kind( cue->node_head, WEBVTT_VOICE );
  lang = find_kind( cue->node_head, WEBVTT_LANG );
  underline = find_kind( cue->node_head, WEBVTT_UNDERLINE );
  cls = find_kind( cue->node_head, WEBVTT_CLASS );
  TASK_CHECK( voice != 0 && lang != 0 && underline != 0 && cls != 0 );
  TASK_CHECK( strcmp( webvtt_string_text(
              &voice->data.internal_data->annotation ), "Bob" ) == 0 );
  TASK_CHECK( cls->data.internal_data->css_classes->length == 2 );
  TASK_CHECK( strcmp( webvtt_string_text(
    &cls->data.internal_data->css_classes->items[ 0 ] ), "a" ) == 0 );
  TASK_CHECK( strcmp( webvtt_string_text(
    &cls->data.internal_data->css_classes->items[ 1 ] ), "b" ) == 0 );
  TASK_CHECK( strcmp( webvtt_string_text(
              &lang->data.internal_data->lang ), "ja" ) == 0 );

  /* The normalized payload round-trips through the streaming parser. */
  describe_tree( cue->node_head, before_desc, sizeof( before_desc ) );
  reread = parse_body( webvtt_string_text( &cue->body ) );
  TASK_CHECK( reread != 0 );
  describe_tree( reread->node_head, after_desc, sizeof( after_desc ) );
  TASK_CHECK( strcmp( before_desc, after_desc ) == 0 );
  visible[ 0 ] = 0;
  collect_text( reread->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "wQz" ) == 0 );
  ldata = find_kind( reread->node_head,
                     WEBVTT_LANG )->data.internal_data;
  TASK_CHECK( strcmp( webvtt_string_text( &ldata->lang ), "ja" ) == 0 );
  TASK_CHECK( find_kind( reread->node_head,
                         WEBVTT_CLASS )->data.internal_data
              ->css_classes->length == 2 );
  webvtt_release_cue( &reread );
  webvtt_release_cue( &cue );
  return 0;
}

static int
selection_line_feeds( void )
{
  webvtt_cue *cue = parse_body( "a\nb" );
  webvtt_cue *reread;
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );

  /* The line feed is one code point: deleting position 1 joins the lines. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 2, "", 0 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "ab" ) == 0 );
  webvtt_release_cue( &cue );

  /* LF is legal inside replacement text and survives a save/reparse. */
  cue = parse_body( "xy" );
  TASK_CHECK( cue != 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 2, "p\nq", 3 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "p\nq" ) == 0 );
  TASK_CHECK( strcmp( webvtt_string_text( &cue->body ), "p\nq" ) == 0 );

  reread = parse_body( webvtt_string_text( &cue->body ) );
  TASK_CHECK( reread != 0 );
  visible[ 0 ] = 0;
  collect_text( reread->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "p\nq" ) == 0 );
  webvtt_release_cue( &reread );
  webvtt_release_cue( &cue );
  return 0;
}

/* ------------------------------------------------------------------ */
/* anchors group                                                       */
/* ------------------------------------------------------------------ */

static int
anchors_shift_with_text( void )
{
  webvtt_cue *cue = parse_body(
    "a<00:00:01.000>b<00:00:02.000>c" );
  struct stamp_walk walk;
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );
  TASK_CHECK( cue->from == 1000 && cue->until == 2000 );

  /* Replace one code point with two: the begin anchor stays put and the
   * anchor after the selection shifts. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 2, "XX", 2 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "aXXc" ) == 0 );

  memset( &walk, 0, sizeof( walk ) );
  walk_stamps( cue->node_head, &walk );
  TASK_CHECK( walk.count == 2 );
  TASK_CHECK( walk.positions[ 0 ] == 1 && walk.values[ 0 ] == 1000 );
  TASK_CHECK( walk.positions[ 1 ] == 3 && walk.values[ 1 ] == 2000 );
  TASK_CHECK( cue->from == 1000 && cue->until == 2000 );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );

  webvtt_release_cue( &cue );
  return 0;
}

static int
anchors_same_position_boundaries( void )
{
  webvtt_cue *cue = parse_body(
    "<00:00:01.000>ab<00:00:02.000>" );
  struct stamp_walk walk;
  webvtt_internal_node_data *hdata;
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );

  /* Inserting at an anchor position keeps the anchor before the new text. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 2, 2, "Q", 1 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "abQ" ) == 0 );

  memset( &walk, 0, sizeof( walk ) );
  walk_stamps( cue->node_head, &walk );
  TASK_CHECK( walk.count == 2 );
  TASK_CHECK( walk.positions[ 0 ] == 0 && walk.values[ 0 ] == 1000 );
  TASK_CHECK( walk.positions[ 1 ] == 2 && walk.values[ 1 ] == 2000 );

  hdata = cue->node_head->data.internal_data;
  TASK_CHECK( hdata->length == 4 );
  TASK_CHECK( hdata->children[ 0 ]->kind == WEBVTT_TIME_STAMP );
  TASK_CHECK( hdata->children[ 1 ]->kind == WEBVTT_TEXT );
  TASK_CHECK( hdata->children[ 2 ]->kind == WEBVTT_TIME_STAMP );
  TASK_CHECK( hdata->children[ 3 ]->kind == WEBVTT_TEXT );
  TASK_CHECK( strcmp( webvtt_string_text(
              &hdata->children[ 3 ]->data.text ), "Q" ) == 0 );
  webvtt_release_cue( &cue );

  /* Delete only "b": both boundary anchors survive and collapse onto the
   * same position, in their original order; the interior region is empty. */
  cue = parse_body( "a<00:00:01.500>b<00:00:02.500>c" );
  TASK_CHECK( cue != 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 2, "", 0 ) ==
              WEBVTT_SUCCESS );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "ac" ) == 0 );
  memset( &walk, 0, sizeof( walk ) );
  walk_stamps( cue->node_head, &walk );
  TASK_CHECK( walk.count == 2 );
  TASK_CHECK( walk.positions[ 0 ] == 1 && walk.values[ 0 ] == 1500 );
  TASK_CHECK( walk.positions[ 1 ] == 1 && walk.values[ 1 ] == 2500 );
  webvtt_release_cue( &cue );
  return 0;
}

static int
anchors_inside_formatting( void )
{
  webvtt_cue *cue = parse_body( "<i>a<00:00:03.000>b</i>c" );
  struct stamp_walk walk;
  webvtt_node *stamp;

  TASK_CHECK( cue != 0 );

  /* Editing outside the italic span leaves the anchored branch and its
   * chain untouched. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 2, 3, "Z", 1 ) ==
              WEBVTT_SUCCESS );
  memset( &walk, 0, sizeof( walk ) );
  walk_stamps( cue->node_head, &walk );
  TASK_CHECK( walk.count == 1 );
  TASK_CHECK( walk.positions[ 0 ] == 1 && walk.values[ 0 ] == 3000 );
  stamp = find_kind( cue->node_head, WEBVTT_TIME_STAMP );
  TASK_CHECK( stamp != 0 );
  TASK_CHECK( stamp->parent != 0 );
  TASK_CHECK( stamp->parent->kind == WEBVTT_ITALIC );
  webvtt_release_cue( &cue );

  /* Removing all text around an interior anchor removes the anchor and the
   * formatting branch that has been emptied. */
  cue = parse_body( "<i>a<00:00:03.000>b</i>c" );
  TASK_CHECK( cue != 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 2, "", 0 ) ==
              WEBVTT_SUCCESS );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_TIME_STAMP ) == 0 );
  TASK_CHECK( count_kind( cue->node_head, WEBVTT_ITALIC ) == 0 );
  {
    char visible[ 256 ];
    visible[ 0 ] = 0;
    collect_text( cue->node_head, visible, sizeof( visible ) );
    TASK_CHECK( strcmp( visible, "c" ) == 0 );
  }
  webvtt_release_cue( &cue );
  return 0;
}

static int
anchors_after_coalesced_text( void )
{
  /* All text shares one root chain, so the surviving runs and the inserted
   * text coalesce into a single TEXT node. Anchor positions must still be
   * measured by code points rather than the coalesced byte accounting. */
  webvtt_cue *cue = parse_body(
    "ab<00:00:05.000>cd<00:00:06.000>ef" );
  struct stamp_walk walk;
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );
  TASK_CHECK( webvtt_cue_replace_text( cue, 2, 2, "Q", 1 ) ==
              WEBVTT_SUCCESS );

  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "abQcdef" ) == 0 );

  memset( &walk, 0, sizeof( walk ) );
  walk_stamps( cue->node_head, &walk );
  TASK_CHECK( walk.count == 2 );
  TASK_CHECK( walk.positions[ 0 ] == 2 && walk.values[ 0 ] == 5000 );
  TASK_CHECK( walk.positions[ 1 ] == 5 && walk.values[ 1 ] == 6000 );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );
  webvtt_release_cue( &cue );
  return 0;
}

/* ------------------------------------------------------------------ */
/* transaction group                                                   */
/* ------------------------------------------------------------------ */

static int
transaction_invalid_arguments( void )
{
  webvtt_cue *cue = parse_body( "abc" );
  const char *saved_body;
  char saved_visible[ 256 ];
  static const char bad_nul[] = { 'a', 0, 'b' };
  static const char bad_cr[] = { 'a', '\r', 'b' };
  static const char bad_byte[] = { (char)0xFF };
  static const char bad_overlong[] = { (char)0xC0, (char)0x80 };
  static const char bad_truncated[] = { (char)0xE4, (char)0xB8 };
  static const char bad_surrogate[] = { (char)0xED, (char)0xA0,
                                        (char)0x80 };

  TASK_CHECK( cue != 0 );
  saved_body = webvtt_string_text( &cue->body );
  saved_visible[ 0 ] = 0;
  collect_text( cue->node_head, saved_visible, sizeof( saved_visible ) );

  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, bad_nul, 3 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, bad_cr, 3 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, bad_byte, 1 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, bad_overlong, 2 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, bad_truncated, 2 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, bad_surrogate, 3 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 4, "x", 1 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 2, 1, "x", 1 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( 0, 0, 1, "x", 1 ) !=
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, 0, 1 ) !=
              WEBVTT_SUCCESS );

  /* Every rejected call left body and tree exactly as they were. */
  TASK_CHECK( strcmp( webvtt_string_text( &cue->body ),
                      saved_body ) == 0 );
  {
    char visible[ 256 ];
    visible[ 0 ] = 0;
    collect_text( cue->node_head, visible, sizeof( visible ) );
    TASK_CHECK( strcmp( visible, saved_visible ) == 0 );
  }

  /* The cue is still editable after the failures. */
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 3, "def", 3 ) ==
              WEBVTT_SUCCESS );
  {
    char visible[ 256 ];
    visible[ 0 ] = 0;
    collect_text( cue->node_head, visible, sizeof( visible ) );
    TASK_CHECK( strcmp( visible, "def" ) == 0 );
  }
  webvtt_release_cue( &cue );
  return 0;
}

/* Controlled allocator: fail exactly one allocation, at call 'fail_at'. */
static unsigned alloc_calls;
static unsigned fail_at;

static void *
controlled_alloc( void *userdata, webvtt_uint nbytes )
{
  (void)userdata;
  ++alloc_calls;
  if( alloc_calls == fail_at ) {
    return 0;
  }
  return malloc( nbytes );
}

static void
controlled_free( void *userdata, void *memory )
{
  (void)userdata;
  free( memory );
}

static int
transaction_allocation_failure( void )
{
  static const char *vtt =
    "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\n"
    "<v Alice><c.red>a<b>b</b></c>c<00:00:01.500>d</v>\n\n";
  unsigned parse_allocations;
  unsigned attempt;
  unsigned observed_oom = 0;

  /* First pass: count the allocations a successful parse needs, with the
   * failing point disabled. No library objects are live at entry. */
  alloc_calls = 0;
  fail_at = 0xFFFFFFFFu;
  webvtt_set_allocator( controlled_alloc, controlled_free, 0 );
  {
    webvtt_cue *probe = parse_vtt( vtt );
    TASK_CHECK( probe != 0 );
    parse_allocations = alloc_calls;
    webvtt_release_cue( &probe );
  }
  webvtt_set_allocator( 0, 0, 0 );

  /* Every fail point after the parse must either hit the edit path (OOM,
   * cue untouched) or lie beyond the edit's needs (success). */
  for( attempt = 1; attempt <= 600; ++attempt ) {
    webvtt_cue *cue;
    webvtt_status status;
    char expected_visible[ 256 ];
    char visible[ 256 ];
    const char *expected_body;

    alloc_calls = 0;
    fail_at = parse_allocations + attempt;
    webvtt_set_allocator( controlled_alloc, controlled_free, 0 );

    cue = parse_vtt( vtt );
    if( !cue ) {
      webvtt_set_allocator( 0, 0, 0 );
      continue;
    }

    expected_body = strdup( webvtt_string_text( &cue->body ) );
    expected_visible[ 0 ] = 0;
    collect_text( cue->node_head, expected_visible,
                  sizeof( expected_visible ) );

    status = webvtt_cue_replace_text( cue, 1, 3, "XY", 2 );
    if( status == WEBVTT_OUT_OF_MEMORY ) {
      ++observed_oom;
      TASK_CHECK( strcmp( webvtt_string_text( &cue->body ),
                          expected_body ) == 0 );
      visible[ 0 ] = 0;
      collect_text( cue->node_head, visible, sizeof( visible ) );
      TASK_CHECK( strcmp( visible, expected_visible ) == 0 );
      TASK_CHECK( parents_valid( cue->node_head, 0 ) );

      /* The failed transaction must not poison the next one. */
      status = webvtt_cue_replace_text( cue, 1, 3, "XY", 2 );
    }

    free( (void *)expected_body );
    webvtt_release_cue( &cue );
    webvtt_set_allocator( 0, 0, 0 );

    if( status == WEBVTT_SUCCESS ) {
      break; /* fail point is past everything the edit allocates */
    }
  }

  TASK_CHECK( observed_oom > 0 );

  /* Final run on the default allocator succeeds with the concrete result. */
  {
    webvtt_cue *cue = parse_vtt( vtt );
    char visible[ 256 ];
    struct stamp_walk walk;

    TASK_CHECK( cue != 0 );
    TASK_CHECK( webvtt_cue_replace_text( cue, 1, 3, "XY", 2 ) ==
                WEBVTT_SUCCESS );
    visible[ 0 ] = 0;
    collect_text( cue->node_head, visible, sizeof( visible ) );
    TASK_CHECK( strcmp( visible, "aXYd" ) == 0 );
    memset( &walk, 0, sizeof( walk ) );
    walk_stamps( cue->node_head, &walk );
    TASK_CHECK( walk.count == 1 );
    TASK_CHECK( walk.values[ 0 ] == 1500 );
    webvtt_release_cue( &cue );
  }
  return 0;
}

static int
transaction_old_root_stays_readable( void )
{
  webvtt_cue *cue = parse_body(
    "<v Alice><c.red>A&amp;<i>中</i></c>B</v>" );
  webvtt_node *old_root;
  char old_visible[ 256 ];
  char new_visible[ 256 ];

  TASK_CHECK( cue != 0 );
  old_root = cue->node_head;
  webvtt_ref_node( old_root );

  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 3, "XX", 2 ) ==
              WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, "Y", 1 ) ==
              WEBVTT_SUCCESS );

  /* The retained root is the pre-edit subtree and still reads correctly. */
  old_visible[ 0 ] = 0;
  collect_text( old_root, old_visible, sizeof( old_visible ) );
  TASK_CHECK( strcmp( old_visible, "A&中B" ) == 0 );
  TASK_CHECK( parents_valid( old_root, 0 ) );
  TASK_CHECK( find_kind( old_root, WEBVTT_ITALIC ) != 0 );

  /* The live tree reflects both edits. */
  new_visible[ 0 ] = 0;
  collect_text( cue->node_head, new_visible, sizeof( new_visible ) );
  TASK_CHECK( strcmp( new_visible, "YXXB" ) == 0 );
  TASK_CHECK( cue->node_head != old_root );

  webvtt_release_node( &old_root );
  webvtt_release_cue( &cue );
  return 0;
}

static int
transaction_save_and_reparse( void )
{
  webvtt_cue *cue = parse_body(
    "<v Alice><c.red>a<b>b</b></c><i>c</i></v><00:00:01.500>d" );
  webvtt_cue *reread;
  char before_desc[ 512 ];
  char after_desc[ 512 ];
  char visible[ 256 ];
  struct stamp_walk walk;
  const char *id_text;

  TASK_CHECK( cue != 0 );
  id_text = strdup( webvtt_string_text( &cue->id ) );

  /* Replace only the bold "b" with a CJK char plus a four-byte emoji, so
   * every formatting branch survives; then replace the leading "a". */
  TASK_CHECK( webvtt_cue_replace_text( cue, 1, 2, "中\xF0\x9F\x98\x80",
                                       7 ) == WEBVTT_SUCCESS );
  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, "Z", 1 ) ==
              WEBVTT_SUCCESS );

  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible,
                      "Z中\xF0\x9F\x98\x80" "cd" ) == 0 );
  TASK_CHECK( strcmp( webvtt_string_text( &cue->id ), id_text ) == 0 );
  TASK_CHECK( cue->from == 1000 && cue->until == 2000 );

  describe_tree( cue->node_head, before_desc, sizeof( before_desc ) );

  reread = parse_body( webvtt_string_text( &cue->body ) );
  TASK_CHECK( reread != 0 );
  describe_tree( reread->node_head, after_desc, sizeof( after_desc ) );
  TASK_CHECK( strcmp( before_desc, after_desc ) == 0 );

  /* Concrete checks on top of the structural comparison. */
  visible[ 0 ] = 0;
  collect_text( reread->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "Z中\xF0\x9F\x98\x80" "cd" ) == 0 );
  memset( &walk, 0, sizeof( walk ) );
  walk_stamps( reread->node_head, &walk );
  TASK_CHECK( walk.count == 1 );
  TASK_CHECK( walk.values[ 0 ] == 1500 );
  TASK_CHECK( find_kind( reread->node_head, WEBVTT_VOICE ) != 0 );
  TASK_CHECK( find_kind( reread->node_head, WEBVTT_CLASS ) != 0 );
  TASK_CHECK( find_kind( reread->node_head, WEBVTT_ITALIC ) != 0 );

  free( (void *)id_text );
  webvtt_release_cue( &reread );
  webvtt_release_cue( &cue );
  return 0;
}

static int
transaction_unsupported_ruby( void )
{
  webvtt_cue *cue = parse_body( "<ruby>a<rt>b</rt></ruby>" );
  const char *saved_body;
  char visible[ 256 ];

  TASK_CHECK( cue != 0 );
  saved_body = strdup( webvtt_string_text( &cue->body ) );

  TASK_CHECK( webvtt_cue_replace_text( cue, 0, 1, "x", 1 ) ==
              WEBVTT_NOT_SUPPORTED );

  TASK_CHECK( strcmp( webvtt_string_text( &cue->body ),
                      saved_body ) == 0 );
  visible[ 0 ] = 0;
  collect_text( cue->node_head, visible, sizeof( visible ) );
  TASK_CHECK( strcmp( visible, "ab" ) == 0 );
  TASK_CHECK( parents_valid( cue->node_head, 0 ) );

  free( (void *)saved_body );
  webvtt_release_cue( &cue );
  return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "selection", "entities_cjk_emoji", selection_entities_cjk_emoji },
  { "selection", "cross_nested_branches",
    selection_cross_nested_branches },
  { "selection", "head_and_tail_insert", selection_head_and_tail_insert },
  { "selection", "clear_and_rebuild", selection_clear_and_rebuild },
  { "selection", "deep_nesting_roundtrip",
    selection_deep_nesting_roundtrip },
  { "selection", "line_feeds", selection_line_feeds },

  { "anchors", "shift_with_text", anchors_shift_with_text },
  { "anchors", "same_position_boundaries",
    anchors_same_position_boundaries },
  { "anchors", "inside_formatting", anchors_inside_formatting },
  { "anchors", "after_coalesced_text", anchors_after_coalesced_text },

  { "transaction", "invalid_arguments", transaction_invalid_arguments },
  { "transaction", "allocation_failure", transaction_allocation_failure },
  { "transaction", "old_root_stays_readable",
    transaction_old_root_stays_readable },
  { "transaction", "save_and_reparse", transaction_save_and_reparse },
  { "transaction", "unsupported_ruby", transaction_unsupported_ruby },
};

const size_t task_case_count =
  sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
