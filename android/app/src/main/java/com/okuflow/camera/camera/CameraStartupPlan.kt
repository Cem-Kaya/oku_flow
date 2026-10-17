package com.okuflow.camera.camera

/** One monotonic startup budget, including discovery, queued work and retry gaps. */
internal class CameraStartupPlan(
    startedAtMs: Long,
    budgetMs: Long,
    candidateCount: Int,
    private val nowMs: () -> Long,
    private val isCurrent: () -> Boolean,
) {
    val budgetMs = budgetMs.coerceIn(1, MAX_STARTUP_MS)
    private val deadlineMs = startedAtMs + this.budgetMs
    // Depleted host budgets should try fewer modes, with useful time per
    // mode, rather than thrashing through six near-zero camera opens.
    val candidateCount = minOf(candidateCount.coerceIn(0, MAX_CANDIDATES),
        ((remainingMs() + RETRY_DELAY_MS) / (MIN_CANDIDATE_MS + RETRY_DELAY_MS)).toInt())

    data class Attempt(val deadlineMs: Long, val timeoutMs: Long)

    fun remainingMs(): Long = (deadlineMs - nowMs()).coerceAtLeast(0)

    fun attempt(index: Int): Attempt? {
        if (!isCurrent() || index !in 0 until candidateCount) return null
        val now = nowMs()
        val left = candidateCount - index
        // Reserve the retry gaps and a fair share for every remaining mode.
        // Fast rejection returns its unused share to the next candidate.
        val available = deadlineMs - now - (left - 1) * RETRY_DELAY_MS
        if (available < left * MIN_CANDIDATE_MS) return null
        val timeout = minOf(MAX_CANDIDATE_MS, available / left)
        return Attempt(now + timeout, timeout)
    }

    fun retryDelayMs(): Long? =
        if (isCurrent() && remainingMs() > RETRY_DELAY_MS) RETRY_DELAY_MS else null

    companion object {
        const val MAX_STARTUP_MS = 12_000L
        const val MAX_CANDIDATES = 6
        const val MAX_CANDIDATE_MS = 4_000L
        const val MIN_CANDIDATE_MS = 500L
        const val RETRY_DELAY_MS = 200L
    }
}
