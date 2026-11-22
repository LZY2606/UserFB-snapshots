//--------------------------------------------//
// SFOTool - SF2 subset/repack                //
// License: Public Domain (www.unlicense.org) //
//--------------------------------------------//

#ifndef SFO_SUBSET_H
#define SFO_SUBSET_H

#include <stddef.h>

// Writes a new soundfont to out_path containing only the presets selected by
// preset_list ("bank:preset[,bank:preset...]"), the instruments/samples they
// reference (plus linked sample partners) and a repacked 16-bit PCM pool.
// Returns 0 on success. On failure returns non-zero, fills errmsg (if given)
// and leaves any pre-existing file at out_path untouched.
int sfo_subset_file(const char* in_path, const char* out_path, const char* preset_list, char* errmsg, size_t errmsg_size);

#endif
