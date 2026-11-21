/*
 * examples/cueselect.c
 *
 * Demonstrates webvtt_cue_replace_text(): a cue is obtained from the
 * streaming parser callback, a selection that crosses formatting branches
 * is replaced by literal text (angle brackets and ampersands stay text),
 * the edited cue is written back out as a complete WebVTT document with its
 * timing line, and the saved document is parsed a second time to prove the
 * normalized payload round-trips.
 *
 * Build against the static library, e.g.
 *   cc -std=c99 -DWEBVTT_STATIC=1 -Iinclude \
 *      examples/cueselect.c src/libwebvtt/.libs/libwebvtt.a -o cueselect
 * or with the task harness:
 *   cc -std=c99 -DWEBVTT_STATIC=1 -I. -Iinclude \
 *      examples/cueselect.c .task-build/libwebvtt.a -o cueselect
 */

#include <webvtt/parser.h>
#include <webvtt/cue.h>
#include <webvtt/node.h>

#include <stdio.h>
#include <string.h>

struct captured {
  webvtt_cue *cue;
  unsigned errors;
};

static void
on_cue( void *userdata, webvtt_cue *cue )
{
  struct captured *captured = (struct captured *)userdata;
  captured->cue = cue;
  webvtt_ref_cue( cue );
  /* Return the reference the parser handed to this callback. */
  webvtt_release_cue( &cue );
}

static int
on_error( void *userdata, webvtt_uint line, webvtt_uint col,
          webvtt_error error )
{
  struct captured *captured = (struct captured *)userdata;
  (void)line;
  (void)col;
  (void)error;
  ++captured->errors;
  return 0;
}

static webvtt_cue *
parse_document( const char *document )
{
  struct captured captured;
  webvtt_parser parser = 0;
  size_t i;

  memset( &captured, 0, sizeof( captured ) );
  if( webvtt_create_parser( on_cue, on_error, &captured,
                            &parser ) != WEBVTT_SUCCESS ) {
    return 0;
  }
  for( i = 0; i < strlen( document ); ++i ) {
    if( WEBVTT_FAILED( webvtt_parse_chunk( parser, document + i, 1 ) ) ) {
      webvtt_delete_parser( parser );
      return 0;
    }
  }
  if( WEBVTT_FAILED( webvtt_finish_parsing( parser ) ) ) {
    webvtt_delete_parser( parser );
    return 0;
  }
  webvtt_delete_parser( parser );
  return captured.errors ? 0 : captured.cue;
}

static void
print_timestamp( webvtt_timestamp millis )
{
  webvtt_uint hours = (webvtt_uint)( millis / 3600000 );
  webvtt_uint minutes = (webvtt_uint)( ( millis % 3600000 ) / 60000 );
  webvtt_uint seconds = (webvtt_uint)( ( millis % 60000 ) / 1000 );
  webvtt_uint fraction = (webvtt_uint)( millis % 1000 );

  printf( "%02u:%02u:%02u.%03u", hours, minutes, seconds, fraction );
}

static void
print_visible_text( webvtt_node *node )
{
  if( node->kind == WEBVTT_TEXT ) {
    fputs( webvtt_string_text( &node->data.text ), stdout );
  } else if( WEBVTT_IS_VALID_INTERNAL_NODE( node->kind ) ) {
    webvtt_internal_node_data *data = node->data.internal_data;
    webvtt_uint i;
    for( i = 0; i < data->length; ++i ) {
      print_visible_text( data->children[ i ] );
    }
  }
}

int
main( void )
{
  static const char *document =
    "WEBVTT\n"
    "\n"
    "id-7\n"
    "00:00:10.000 --> 00:00:14.000 align:start\n"
    "<v Alice><c.red>A&amp;<i>\xE4\xB8\xAD\xF0\x9F\x98\x80</i></c>"
    "B</v>\n"
    "\n";
  webvtt_cue *cue;
  webvtt_cue *reread;
  char saved[ 2048 ];
  const char *replacement = "X<Y&Z"; /* literal text, never markup */

  cue = parse_document( document );
  if( !cue ) {
    fputs( "initial parse failed\n", stderr );
    return 1;
  }

  printf( "cue id    : %s\n", webvtt_string_text( &cue->id ) );
  fputs( "before    : ", stdout );
  print_visible_text( cue->node_head );
  putchar( '\n' );

  /* Visible text is A & 中 😀 B: replace "&中😀" (code points 1..4),
   * which spans the class branch and its nested italic branch. */
  if( webvtt_cue_replace_text( cue, 1, 4, replacement,
                               (webvtt_uint)strlen( replacement ) ) !=
      WEBVTT_SUCCESS ) {
    fputs( "replace failed\n", stderr );
    return 2;
  }

  fputs( "after     : ", stdout );
  print_visible_text( cue->node_head );
  putchar( '\n' );

  /* Serialize a complete, saveable WebVTT document from the edited cue. */
  snprintf( saved, sizeof( saved ),
            "WEBVTT\n\n%s\n",
            webvtt_string_text( &cue->id ) );
  fputs( saved, stdout );
  print_timestamp( cue->from );
  fputs( " --> ", stdout );
  print_timestamp( cue->until );
  putchar( '\n' );
  puts( webvtt_string_text( &cue->body ) );
  putchar( '\n' );

  /* Re-read the saved document: tree text and markup must survive. */
  snprintf( saved, sizeof( saved ),
            "WEBVTT\n\n00:00:10.000 --> 00:00:14.000\n%s\n\n",
            webvtt_string_text( &cue->body ) );
  reread = parse_document( saved );
  if( !reread ) {
    fputs( "reparse failed\n", stderr );
    return 3;
  }
  fputs( "reparsed  : ", stdout );
  print_visible_text( reread->node_head );
  putchar( '\n' );
  printf( "reparsed body: %s\n", webvtt_string_text( &reread->body ) );

  webvtt_release_cue( &reread );
  webvtt_release_cue( &cue );
  return 0;
}
