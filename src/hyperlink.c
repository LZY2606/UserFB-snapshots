#include "vterm_internal.h"

#include <string.h>

/* OSC 8 hyperlink identity table.
 *
 * Each VTerm owns a table of hyperlink targets. An identity (a nonzero
 * uint64_t) is handed out exactly once per VTerm lifetime. An entry exists
 * precisely while it has at least one reference:
 *   - internal references are held by the active pen in the state layer and
 *     by every screen cell that carries the identity;
 *   - external references are taken by callers via vterm_hyperlink_retain().
 * When the last reference goes away the entry and its string storage are
 * freed, so old scrollback does not pin every historical URI forever.
 */

static VTermHyperlink *find_entry(const VTerm *vt, uint64_t id)
{
  for(VTermHyperlink *e = vt->hyperlinks.entries; e; e = e->next)
    if(e->id == id)
      return e;
  return NULL;
}

static VTermHyperlink *find_reusable(const VTerm *vt,
    const char *ext_id, size_t ext_id_len, const char *uri, size_t uri_len)
{
  /* Only links with a nonempty explicit id= are eligible for reuse */
  if(!ext_id_len)
    return NULL;

  for(VTermHyperlink *e = vt->hyperlinks.entries; e; e = e->next)
    if(e->ext_id_len == ext_id_len &&
       e->uri_len    == uri_len    &&
       memcmp(e->ext_id, ext_id, ext_id_len) == 0 &&
       memcmp(e->uri,    uri,    uri_len)    == 0)
      return e;

  return NULL;
}

static void free_entry(VTerm *vt, VTermHyperlink *e)
{
  vterm_allocator_free(vt, e->uri);
  vterm_allocator_free(vt, e->ext_id);
  vterm_allocator_free(vt, e);
}

static void maybe_free_entry(VTerm *vt, VTermHyperlink *e)
{
  if(e->internal_refs || e->external_refs)
    return;

  VTermHyperlink **link = &vt->hyperlinks.entries;
  while(*link && *link != e)
    link = &(*link)->next;
  if(*link)
    *link = e->next;

  free_entry(vt, e);
}

uint64_t vterm_hyperlink_intern(VTerm *vt,
    const char *ext_id, size_t ext_id_len, const char *uri, size_t uri_len)
{
  VTermHyperlink *e = find_reusable(vt, ext_id, ext_id_len, uri, uri_len);
  if(e)
    return e->id;

  e = vterm_allocator_malloc(vt, sizeof(VTermHyperlink));

  e->id            = vt->hyperlinks.next_id++;
  e->internal_refs = 0;
  e->external_refs = 0;
  e->uri_len       = uri_len;
  e->ext_id_len    = ext_id_len;

  e->uri = vterm_allocator_malloc(vt, uri_len + 1);
  memcpy(e->uri, uri, uri_len);
  e->uri[uri_len] = 0;

  e->ext_id = vterm_allocator_malloc(vt, ext_id_len + 1);
  memcpy(e->ext_id, ext_id, ext_id_len);
  e->ext_id[ext_id_len] = 0;

  e->next = vt->hyperlinks.entries;
  vt->hyperlinks.entries = e;

  return e->id;
}

void vterm_hyperlink_retain_internal(VTerm *vt, uint64_t id)
{
  if(!id)
    return;

  VTermHyperlink *e = find_entry(vt, id);
  if(e)
    e->internal_refs++;
}

void vterm_hyperlink_release_internal(VTerm *vt, uint64_t id)
{
  if(!id)
    return;

  VTermHyperlink *e = find_entry(vt, id);
  if(!e || !e->internal_refs)
    return;

  e->internal_refs--;
  maybe_free_entry(vt, e);
}

void vterm_hyperlink_destroy_all(VTerm *vt)
{
  VTermHyperlink *e = vt->hyperlinks.entries;
  while(e) {
    VTermHyperlink *next = e->next;
    free_entry(vt, e);
    e = next;
  }
  vt->hyperlinks.entries = NULL;
}

int vterm_hyperlink_get(const VTerm *vt, uint64_t id, const char **uri, const char **external_id)
{
  if(!id)
    return 0;

  VTermHyperlink *e = find_entry(vt, id);
  if(!e)
    return 0;

  if(uri)
    *uri = e->uri;
  if(external_id)
    *external_id = e->ext_id;

  return 1;
}

int vterm_hyperlink_retain(VTerm *vt, uint64_t id)
{
  if(!id)
    return 0;

  VTermHyperlink *e = find_entry(vt, id);
  if(!e)
    return 0;

  e->external_refs++;
  return 1;
}

int vterm_hyperlink_release(VTerm *vt, uint64_t id)
{
  if(!id)
    return 0;

  VTermHyperlink *e = find_entry(vt, id);
  if(!e || !e->external_refs)
    /* No external reference to release; never touch internal ones */
    return 0;

  e->external_refs--;
  maybe_free_entry(vt, e);
  return 1;
}
