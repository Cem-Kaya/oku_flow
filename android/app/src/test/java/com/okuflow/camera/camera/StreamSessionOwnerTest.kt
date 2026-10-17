package com.okuflow.camera.camera

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicReference

class StreamSessionOwnerTest {
    private class Fixture {
        var live: String? = "A"
        val stops = mutableListOf<String>()
        val starts = mutableListOf<String>()
        val owner = StreamSessionOwner({ it == live }, { stops.add(it) })
        init { assertTrue(owner.bind("A")); assertTrue(owner.start("A")) }
        fun restart() = owner.restart { starts.add(it) }
    }

    @Test fun delayedSettingsRestartCannotPassAStop() {
        val f = Fixture()
        val ready = CountDownLatch(1)
        val release = CountDownLatch(1)
        val delayed = AtomicReference<() -> Unit>()
        val producer = Thread {
            // Equivalent to a settings callback waiting before posting to the service owner.
            ready.countDown(); release.await()
            delayed.set { f.restart() }
        }.also { it.start() }
        ready.await()
        assertTrue(f.owner.stop("A"))
        release.countDown(); producer.join()
        delayed.get().invoke()
        assertTrue(f.starts.isEmpty())
        assertEquals(listOf("A"), f.stops)
    }

    @Test fun admittedStartThenStopStillRetiresTheOnlyEngine() {
        val f = Fixture()
        f.restart()
        f.owner.stop("A")
        f.restart()
        assertEquals(listOf("A"), f.starts)
        assertFalse(f.owner.streaming("A"))
        assertEquals(listOf("A"), f.stops)
    }

    @Test fun oldCommandsAndRetirementCannotAffectSuccessor() {
        val f = Fixture()
        val oldCommand = { if (f.owner.accepts("A")) f.owner.stop("A") }
        f.live = "B"
        assertTrue(f.owner.bind("B"))
        assertTrue(f.owner.start("B"))
        oldCommand()
        assertFalse(f.owner.retire("A"))
        assertFalse(f.owner.start("A"))
        f.restart()
        assertEquals(listOf("B"), f.starts)
        assertEquals(listOf("A"), f.stops)
        assertTrue(f.owner.streaming("B"))
    }

    @Test fun delayedOldConnectCannotReplaceCurrentHost() {
        val f = Fixture()
        f.live = "B"
        assertTrue(f.owner.bind("B"))
        assertFalse(f.owner.bind("A"))
        assertEquals("B", f.owner.sessionId)
        f.restart()
        assertTrue(f.starts.isEmpty()) // B requires its own explicit Start.
    }

    @Test fun socketLossRevokesCommandsBeforeQueuedDisconnectRuns() {
        val f = Fixture()
        f.live = null
        assertFalse(f.owner.accepts("A"))
        assertFalse(f.owner.streaming("A"))
        f.restart()
        assertTrue(f.starts.isEmpty())
        assertTrue(f.owner.retire("A"))
        assertNull(f.owner.sessionId)
    }

    @Test fun closeIsTerminalAndStopsOnlyOnce() {
        val f = Fixture()
        f.owner.close(); f.owner.close()
        assertFalse(f.owner.bind("A"))
        assertFalse(f.owner.start("A"))
        f.restart()
        assertTrue(f.starts.isEmpty())
        assertEquals(listOf("A"), f.stops)
    }

    @Test fun callbacksCannotRunLifecycleOnSocketThreads() {
        val f = Fixture()
        val error = AtomicReference<Throwable>()
        Thread { runCatching { f.owner.stop("A") }.exceptionOrNull()?.let { error.set(it) } }.also { it.start(); it.join() }
        assertTrue(error.get() is IllegalStateException)
        assertTrue(f.owner.streaming("A"))
        assertTrue(f.stops.isEmpty())
    }
}
