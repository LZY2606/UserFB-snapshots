package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi

/**
 * Thrown when a checkpoint cannot be captured or restored.
 *
 * Typical causes:
 * - capture requested while the tokenizer is inside an entity, comment, CDATA
 *   or raw-text element (no safe point available at the current position),
 * - the retained buffer would exceed [KsoupCheckpoint.MAX_BUFFER_CHARS],
 * - a serialized checkpoint has an unknown/unsupported format version,
 * - options or encoding differ between capture and restore,
 * - the consumed-prefix digest does not match on resume.
 */
@ExperimentalKsoupApi
public class KsoupCheckpointException(
    message: String,
    cause: Throwable? = null,
) : RuntimeException(message, cause)
