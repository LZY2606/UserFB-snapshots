/*
Copyright (c) 2013, Ralf Willenbacher
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

/* Internal entry points of the optional per-GOB budget mode. */

#ifndef __H261_GOB_BUDGET_H__
#define __H261_GOB_BUDGET_H__

/* nonzero while the GOB budget mode is active on the context */
Int32 h261_gob_budget_active( h261_context_t *ps_ctx );

/* release any lazily allocated scratch storage (safe when unused) */
Void h261_gob_budget_deinit( h261_context_t *ps_ctx );

/* called once per picture, before the picture header is written */
Void h261_gob_budget_init_picture( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture );

/* budget aware replacement for h261_encode_gob() */
Void h261_gob_budget_encode_gob( h261_context_t *ps_ctx, Int32 i_gob_index, Int32 i_offset_x, Int32 i_offset_y );

#endif
