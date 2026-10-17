package com.okuflow.camera.camera

import org.junit.Assert.*
import org.junit.Test

class CameraStartupPlanTest {
    @Test fun stalledPreferredModeLeavesTimeForTheNextModeToSucceed() {
        var now = 0L
        val plan = CameraStartupPlan(0, 11_000, 2, { now }, { true })
        val preferred = requireNotNull(plan.attempt(0))
        assertEquals(4_000L, preferred.timeoutMs)
        // The real first attempt's watchdog fires; selection resumes after its gap.
        now = preferred.deadlineMs
        now += requireNotNull(plan.retryDelayMs())
        val fallback = requireNotNull(plan.attempt(1))
        now = fallback.deadlineMs - 1
        assertTrue(now < 11_000) // Phone video precedes the host's decoder reserve.
        assertEquals(4_000L, fallback.timeoutMs)
        assertTrue(plan.remainingMs() > 0)
    }

    @Test fun allSixCandidatesFitTheActualRemainingHostBudget() {
        for (budget in listOf(4_000L, 6_000L, 11_000L, 12_000L)) {
            var now = 0L
            val plan = CameraStartupPlan(0, budget, 80, { now }, { true })
            assertEquals(6, plan.candidateCount)
            repeat(plan.candidateCount) { index ->
                val attempt = requireNotNull(plan.attempt(index))
                assertTrue(attempt.timeoutMs in 500L..4_000L)
                now = attempt.deadlineMs
                if (index + 1 < plan.candidateCount) now += requireNotNull(plan.retryDelayMs())
            }
            assertTrue("Six modes exceeded $budget ms: $now", now <= budget)
            assertNull(plan.attempt(6))
        }
    }

    @Test fun oneOrTwoCandidatesRetainTheFourSecondWatchdog() {
        for (count in 1..2) {
            var now = 0L
            val plan = CameraStartupPlan(0, 12_000, count, { now }, { true })
            repeat(count) { index ->
                val attempt = requireNotNull(plan.attempt(index))
                assertEquals(4_000L, attempt.timeoutMs)
                now = attempt.deadlineMs
                if (index + 1 < count) now += requireNotNull(plan.retryDelayMs())
            }
        }
    }

    @Test fun fastRejectionReturnsUnusedTimeToLaterModes() {
        var now = 0L
        val plan = CameraStartupPlan(0, 11_000, 6, { now }, { true })
        val first = requireNotNull(plan.attempt(0))
        now = 100 + requireNotNull(plan.retryDelayMs())
        val second = requireNotNull(plan.attempt(1))
        assertTrue(second.timeoutMs > first.timeoutMs)
        assertTrue(second.deadlineMs <= 11_000)
    }

    @Test fun ownerQueueDiscoveryAndTeardownDoNotRenewTheBudget() {
        var now = 2_000L
        val plan = CameraStartupPlan(0, 6_000, 2, { now }, { true })
        val first = requireNotNull(plan.attempt(0))
        assertEquals(1_900L, first.timeoutMs)
        now = first.deadlineMs + 1_000 // Slow teardown/dispatch consumes the same budget.
        now += requireNotNull(plan.retryDelayMs())
        val second = requireNotNull(plan.attempt(1))
        assertEquals(6_000L, second.deadlineMs)
        assertEquals(900L, second.timeoutMs)
        now = 6_000
        assertNull(plan.attempt(1))
        assertNull(plan.retryDelayMs())
    }

    @Test fun retiredStartCannotLaunchAQueuedFallback() {
        var now = 0L
        var currentToken = 7
        val plan = CameraStartupPlan(0, 11_000, 2, { now }, { currentToken == 7 })
        now = requireNotNull(plan.attempt(0)).deadlineMs
        val retry = requireNotNull(plan.retryDelayMs())
        currentToken = 8 // stop, close or replacement start retires token 7 immediately.
        now += retry
        assertNull(plan.attempt(1))
        assertNull(plan.retryDelayMs())
        val successor = CameraStartupPlan(now, 11_000, 2, { now }, { currentToken == 8 })
        assertNotNull(successor.attempt(0))
        assertNull(plan.attempt(1))
    }

    @Test fun expiredOrUnusableBudgetsNeverStartAnotherMode() {
        var now = 12_000L
        val expired = CameraStartupPlan(0, Long.MAX_VALUE, 2, { now }, { true })
        assertEquals(12_000L, expired.budgetMs)
        assertNull(expired.attempt(0))
        now = 0
        val tooShort = CameraStartupPlan(0, 499, 6, { now }, { true })
        assertNull(tooShort.attempt(0))
        assertNull(CameraStartupPlan(0, 12_000, 0, { now }, { true }).attempt(0))
    }

    @Test fun depletedBudgetsTryFewerModesWithUsefulTimeInsteadOfTinyAttempts() {
        var now = 0L
        val plan = CameraStartupPlan(0, 1_500, 6, { now }, { true })
        assertEquals(2, plan.candidateCount)
        val first = requireNotNull(plan.attempt(0))
        assertEquals(650L, first.timeoutMs)
        now = first.deadlineMs + requireNotNull(plan.retryDelayMs())
        val second = requireNotNull(plan.attempt(1))
        assertEquals(650L, second.timeoutMs)
        assertEquals(1_500L, second.deadlineMs)
        assertNull(plan.attempt(2))
    }
}
