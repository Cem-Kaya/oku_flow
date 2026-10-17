package com.okuflow.camera.settings

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Test

class CameraSettingsStateTest {
    @Test fun staleUiPatchAndHostPatchPreserveIndependentFieldsInBothOrders() {
        for (uiFirst in listOf(true, false)) {
            val store = CameraSettingsState(CameraSettings()) {}
            // This callback can outlive the Compose snapshot which rendered
            // the control. It carries only the changed field's value.
            val uiPatch: (CameraSettings) -> CameraSettings = { it.copy(exposure = 3) }
            val hostPatch: (CameraSettings) -> CameraSettings = { it.copy(torch = true) }
            if (uiFirst) { store.update(uiPatch); store.update(hostPatch) }
            else { store.update(hostPatch); store.update(uiPatch) }
            assertEquals(3, store.state.value.exposure)
            assertTrue(store.state.value.torch)
            assertEquals(2L, store.revision())
            store.update { it.forNewHostSession() }
            assertEquals(3, store.state.value.exposure)
            assertFalse(store.state.value.torch)
        }
    }

    @Test fun readModifyPersistPublishIsOneSerializedBoundary() {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val attempted = CountDownLatch(1)
        val failure = AtomicReference<Throwable?>()
        val writes = mutableListOf<CameraSettings>()
        val store = CameraSettingsState(CameraSettings()) { value ->
            if (writes.isEmpty()) {
                entered.countDown()
                check(release.await(2, TimeUnit.SECONDS))
            }
            writes += value
        }
        val first = Thread {
            try { store.update { it.copy(exposure = 4) } } catch (error: Throwable) { failure.set(error) }
        }
        val second = Thread {
            attempted.countDown()
            try { store.update { it.copy(autofocus = false) } } catch (error: Throwable) { failure.set(error) }
        }
        first.start()
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            second.start()
            assertTrue(attempted.await(2, TimeUnit.SECONDS))
        } finally {
            release.countDown()
            first.join(2000)
            if (second.state != Thread.State.NEW) second.join(2000)
        }
        assertFalse(first.isAlive)
        assertFalse(second.isAlive)
        failure.get()?.let { throw it }
        assertEquals(4, store.state.value.exposure)
        assertFalse(store.state.value.autofocus)
        assertEquals(store.state.value, writes.last())
        assertEquals(2L, store.revision())
    }

    @Test fun sameFieldUsesLastAcceptedPatchAndFailedPersistenceDoesNotPublish() {
        var reject = true
        var writes = 0
        val store = CameraSettingsState(CameraSettings()) {
            if (reject) error("fixture persistence refusal")
            writes++
        }
        try { store.update { it.copy(exposure = 2) }; fail("Published a rejected patch") }
        catch (_: IllegalStateException) { }
        assertEquals(0, store.state.value.exposure)
        assertEquals(0L, store.revision())
        reject = false
        store.update { it.copy(exposure = 2) }
        store.update { it.copy(exposure = -1) }
        store.update { it.copy(exposure = -1) }
        assertEquals(-1, store.state.value.exposure)
        assertEquals(2, writes)
        assertEquals(2L, store.revision())
    }
}
