/**
 * Copyright 2008-2012 Digital Bazaar, Inc.
 *
 * This file is part of librdfa.
 *
 * librdfa is Free Software, and can be licensed under any of the
 * following three licenses:
 *
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any
 *      newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 *
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 *
 * See LICENSE-* at the top of this software distribution for more
 * information regarding the details of each license.
 *
 * Internal support for HTML+RDFa 1.1 property copying.  The state holds
 * the default graph while a document is being parsed and performs the
 * rdfa:copy closure once the input has been fully supplied.
 */
#ifndef _RDFA_PROPERTY_COPY_H_
#define _RDFA_PROPERTY_COPY_H_

#include "rdfa.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct rdfa_property_copy_state rdfa_property_copy_state;

/**
 * Creates an empty property-copying graph state.
 *
 * @return the new state, or NULL if memory allocation failed.
 */
rdfa_property_copy_state* rdfa_property_copy_state_create(void);

/**
 * Frees the state and any triples that have not been handed to a
 * callback yet.  Triples already delivered are owned by the callback
 * and are never touched here.
 *
 * @param state the state to free.
 */
void rdfa_property_copy_state_free(rdfa_property_copy_state* state);

/**
 * Adds a triple to the buffered default graph.  The state takes
 * ownership of the triple.  Triples that are already present in the
 * graph (using RDF term equality) are discarded.
 *
 * @param state the property-copying state.
 * @param triple the triple to add.
 */
void rdfa_property_copy_collect(
   rdfa_property_copy_state* state, rdftriple* triple);

/**
 * Applies the rdfa:copy closure, removes pattern definitions used by
 * copy relations, and delivers every remaining triple exactly once.
 *
 * @param state the property-copying state.
 * @param callback the default graph triple handler.
 * @param callback_data user data for the handler.
 */
void rdfa_property_copy_run(
   rdfa_property_copy_state* state, triple_handler_fp callback,
   void* callback_data);

/**
 * @return non-zero once the graph has been delivered.
 */
int rdfa_property_copy_is_delivered(
   const rdfa_property_copy_state* state);

#ifdef __cplusplus
}
#endif

#endif
