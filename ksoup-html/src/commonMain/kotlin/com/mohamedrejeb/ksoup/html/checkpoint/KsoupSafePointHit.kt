package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi

/**
 * A safe point reached during tokenization, with the absolute char boundary
 * just past the last consumed character.
 */
@ExperimentalKsoupApi
public data class KsoupSafePointHit(
    val safePoint: KsoupSafePoint,
    val boundary: Int,
)
