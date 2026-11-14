#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include "h261_decl.h"
int fixture_init(h261_context_t *c,int format,int mode);
void fixture_fill(h261_context_t *c,int frame);
unsigned fixture_encode(h261_context_t *c,int frame,FILE *out);
void fixture_free(h261_context_t *c);
#endif
