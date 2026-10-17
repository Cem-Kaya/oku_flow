package com.okuflow.camera.camera

import org.junit.Assert.*
import org.junit.Test

class SessionZoomIntentTest {
    @Test fun earlyZoomWaitsForCaptureReadinessAndSurvivesFallback() {
        val zoom = SessionZoomIntent()
        zoom.begin("host-a", 7)
        var requests = 0
        assertTrue(zoom.set("host-a", 7, 2.5f, false) { error("No Camera2 session yet") })
        assertEquals(2.5f, zoom.value("host-a", 7), 0f)
        assertTrue(zoom.set("host-a", 7, 3f, false) { error("Fallback candidate has no session") })
        assertEquals(3f, zoom.value("host-a", 7), 0f)
        // The next candidate belongs to the same start and reads this intent
        // when its configured callback builds the first repeating request.
        assertTrue(zoom.set("host-a", 7, zoom.value("host-a", 7), true) { requests++ })
        assertEquals(1, requests)
    }

    @Test fun retiredGenerationAndForeignHostCannotChangeTheSuccessor() {
        val zoom = SessionZoomIntent()
        zoom.begin("host-a", 7)
        zoom.set("host-a", 7, 3f, false) {}
        zoom.begin("host-b", 8)
        assertFalse(zoom.set("host-a", 7, 4f, true) { error("Retired request applied") })
        assertFalse(zoom.set("host-a", 8, 4f, true) { error("Foreign request applied") })
        assertEquals(1f, zoom.value("host-b", 8), 0f)
        assertTrue(zoom.set("host-b", 8, 1.5f, true) {})
        assertEquals(1.5f, zoom.value("host-b", 8), 0f)
    }
}
