package com.okuflow.camera.link

import org.junit.Assert.*
import org.junit.Test

class LinkPolicyTest {
    @org.junit.Test fun transientProblemsRetryButPermanentProblemsRemainVisible() {
        org.junit.Assert.assertTrue(canRetryProblem(HostProblem.CameraBusy, HostLinkState.Disconnected, 30_000))
        org.junit.Assert.assertTrue(canRetryProblem(HostProblem.CameraFailed, HostLinkState.Disconnected, 30_000))
        org.junit.Assert.assertFalse(canRetryProblem(HostProblem.CameraBusy, HostLinkState.Disconnected, 29_999))
        org.junit.Assert.assertFalse(canRetryProblem(HostProblem.CameraBusy, HostLinkState.Connected, 30_000))
        for (problem in listOf(HostProblem.CameraBlocked, HostProblem.TooHot, HostProblem.NoCompatibleMode))
            org.junit.Assert.assertFalse(canRetryProblem(problem, HostLinkState.Disconnected, 30_000))
    }

    @Test fun problemCodesAreStableForHostBackoff() {
        assertEquals(listOf("busy", "blocked", "noMode", "failed", "hot", "failed"),
            HostProblem.entries.map { it.wireCode })
        assertEquals("sessionBusy", SESSION_BUSY_CODE)
    }
    @Test fun unboundControlSessionExpiresAfterBindDeadline() {
        assertFalse(videoBindExpired(1_000, 1_000 + VIDEO_BIND_DEADLINE_MS, videoBound = false))
        assertTrue(videoBindExpired(1_000, 1_001 + VIDEO_BIND_DEADLINE_MS, videoBound = false))
        // A bound session lives on pings, not the bind deadline.
        assertFalse(videoBindExpired(1_000, 60_000, videoBound = true))
    }
    @Test fun onlyShellOrRootPeersAndUnreadableCredentialsAreRefused() {
        assertTrue(peerAllowed(2000))
        assertTrue(peerAllowed(0))
        assertFalse(peerAllowed(10123))
        assertFalse(peerAllowed(null))
    }
}
