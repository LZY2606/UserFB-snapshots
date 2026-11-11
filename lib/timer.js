var GL_QUERY_RESULT_EXT = 0x8866
var GL_QUERY_RESULT_AVAILABLE_EXT = 0x8867
var GL_TIME_ELAPSED_EXT = 0x88BF
var GL_GPU_DISJOINT_EXT = 0x8FBB

module.exports = function (gl, extensions) {
  var ext = extensions.ext_disjoint_timer_query
  if (!ext) {
    return null
  }

  // QUERY POOL BEGIN
  var queryPool = []
  function allocQuery () {
    return queryPool.pop() || ext.createQueryEXT()
  }
  function freeQuery (query) {
    queryPool.push(query)
  }
  // QUERY POOL END

  var pendingQueries = []
  function beginQuery (stats) {
    var query = allocQuery()
    ext.beginQueryEXT(GL_TIME_ELAPSED_EXT, query)
    pendingQueries.push(query)
    pushScopeStats(pendingQueries.length - 1, pendingQueries.length, stats)
  }

  function endQuery () {
    ext.endQueryEXT(GL_TIME_ELAPSED_EXT)
  }

  //
  // Pending stats pool.
  //
  function PendingStats () {
    this.startQueryIndex = -1
    this.endQueryIndex = -1
    this.sum = 0
    this.stats = null
  }
  var pendingStatsPool = []
  function allocPendingStats () {
    return pendingStatsPool.pop() || new PendingStats()
  }
  function freePendingStats (pendingStats) {
    pendingStats.startQueryIndex = -1
    pendingStats.endQueryIndex = -1
    pendingStats.sum = 0
    pendingStats.stats = null
    pendingStatsPool.push(pendingStats)
  }
  // Pending stats pool end

  var pendingStats = []
  function pushScopeStats (start, end, stats) {
    var ps = allocPendingStats()
    ps.startQueryIndex = start
    ps.endQueryIndex = end
    ps.sum = 0
    ps.stats = stats
    pendingStats.push(ps)
  }

  //
  // End the attribution of every unsettled stats record.  Each record
  // that still covers at least one unresolved query is counted once as
  // dropped on its stats object, and any partially accumulated scope sum
  // is discarded along with it: a truncated value must never be
  // published as a complete scope time.  Empty scopes cover no queries,
  // so they are released without counting.  Records are reset before
  // going back to the pool so that queries submitted later can never be
  // attributed to work from before the drop.
  //
  function dropPendingStats () {
    for (var i = 0; i < pendingStats.length; ++i) {
      var ps = pendingStats[i]
      if (ps.startQueryIndex !== ps.endQueryIndex) {
        ps.stats.gpuTimeDropped += 1
      }
      freePendingStats(ps)
    }
    pendingStats.length = 0
  }

  // we should call this at the beginning of the frame,
  // in order to update gpuTime
  var timeSum = []
  var queryPtr = []
  function update () {
    var ptr, i

    // A disjoint event invalidates every pending query result, so it
    // must be checked before reading anything.  Recycle the pending
    // queries without reading them and drop all unsettled stats
    // records.  Timing resumes with whatever is submitted after the
    // disjoint flag clears.
    if (gl.getParameter(ext.GPU_DISJOINT_EXT || GL_GPU_DISJOINT_EXT)) {
      queryPool.push.apply(queryPool, pendingQueries)
      pendingQueries.length = 0
      dropPendingStats()
      return
    }

    var n = pendingQueries.length
    if (n === 0) {
      return
    }

    // Reserve space
    queryPtr.length = Math.max(queryPtr.length, n + 1)
    timeSum.length = Math.max(timeSum.length, n + 1)
    timeSum[0] = 0
    queryPtr[0] = 0

    // Update all pending timer queries
    var queryTime = 0
    ptr = 0
    for (i = 0; i < pendingQueries.length; ++i) {
      var query = pendingQueries[i]
      if (ext.getQueryObjectEXT(query, GL_QUERY_RESULT_AVAILABLE_EXT)) {
        queryTime += ext.getQueryObjectEXT(query, GL_QUERY_RESULT_EXT)
        freeQuery(query)
      } else {
        pendingQueries[ptr++] = query
      }
      timeSum[i + 1] = queryTime
      queryPtr[i + 1] = ptr
    }
    pendingQueries.length = ptr

    // Update all pending stat queries
    ptr = 0
    for (i = 0; i < pendingStats.length; ++i) {
      var stats = pendingStats[i]
      var start = stats.startQueryIndex
      var end = stats.endQueryIndex
      stats.sum += timeSum[end] - timeSum[start]
      var startPtr = queryPtr[start]
      var endPtr = queryPtr[end]
      if (endPtr === startPtr) {
        stats.stats.gpuTime += stats.sum / 1e6
        freePendingStats(stats)
      } else {
        stats.startQueryIndex = startPtr
        stats.endQueryIndex = endPtr
        pendingStats[ptr++] = stats
      }
    }
    pendingStats.length = ptr
  }

  return {
    beginQuery: beginQuery,
    endQuery: endQuery,
    pushScopeStats: pushScopeStats,
    update: update,
    getNumPendingQueries: function () {
      return pendingQueries.length
    },
    clear: function () {
      // Delete every live query exactly once: both the ones still
      // awaiting results and the ones sitting in the pool.
      queryPool.push.apply(queryPool, pendingQueries)
      for (var i = 0; i < queryPool.length; i++) {
        ext.deleteQueryEXT(queryPool[i])
      }
      pendingQueries.length = 0
      queryPool.length = 0
      dropPendingStats()
    },
    restore: function () {
      // The context is lost and the old extension handle is dead, so
      // don't query or delete anything.  Just release all pending
      // queries and end the attribution of every unsettled record.
      pendingQueries.length = 0
      queryPool.length = 0
      dropPendingStats()
    }
  }
}
