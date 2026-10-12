package com.okuflow.camera.camera

import org.junit.Assert.*
import org.junit.Test

class ServiceLifecycleTest {
    @Test fun listenerStartupFailureBeforeRegistrationNeverRemovesIt() {
        // The USB socket can fail after PowerManager is acquired but before thermal add.
        val registration = ListenerRegistration({ fail("Startup never reached thermal registration") },
            { throw IllegalArgumentException("Thermal status listener was not added") })
        registration.close()
        registration.close()
    }

    @Test fun failedThermalRegistrationDoesNotOwnTheListener() {
        var removals = 0
        val registration = ListenerRegistration({ throw IllegalStateException("Registration refused") }, { removals++ })
        assertTrue(runCatching { registration.register() }.isFailure)
        registration.close()
        assertEquals(0, removals)
    }

    @Test fun successfulListenerIsRegisteredAndRemovedExactlyOnce() {
        var additions = 0
        var removals = 0
        val registration = ListenerRegistration({ additions++ }, { removals++ })
        registration.register()
        registration.register()
        registration.close()
        registration.close()
        assertEquals(1, additions)
        assertEquals(1, removals)
    }

    @Test fun failedRemovalIsNotRepeatedDuringPartialTeardown() {
        var removals = 0
        val registration = ListenerRegistration({}, { removals++; throw IllegalStateException("Binder unavailable") })
        registration.register()
        assertTrue(runCatching { registration.close() }.isFailure)
        registration.close()
        assertEquals(1, removals)
    }

    @Test fun permissionReturnWhilePausedCannotStartCameraService() {
        val gate = VisibleServiceStartGate()
        assertFalse(gate.canStart)
        gate.onResumed()
        assertTrue(gate.canStart)
        gate.onPaused()
        assertFalse(gate.canStart)
        gate.onResumed()
        assertTrue(gate.canStart)
    }

    @Test fun rejectedForegroundStartCannotLoopUntilAnotherResume() {
        val gate = VisibleServiceStartGate()
        gate.onResumed()
        gate.onRefused()
        repeat(10) { assertFalse(gate.canStart) }
        gate.onPaused()
        assertFalse(gate.canStart)
        gate.onResumed()
        assertTrue(gate.canStart)
    }
}
