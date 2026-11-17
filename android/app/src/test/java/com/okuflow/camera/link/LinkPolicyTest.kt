package com.okuflow.camera.link

import org.junit.Assert.*
import org.junit.Test

class LinkPolicyTest {
    // Deliberately shortened from 30 s with the computer's reconnect ladder:
    // a stale busy/failed answer must not hold off a fresh host attempt long.
    @org.junit.Test fun transientProblemsRetryButPermanentProblemsRemainVisible() {
        org.junit.Assert.assertTrue(canRetryProblem(HostProblem.CameraBusy, HostLinkState.Disconnected, 10_000))
        org.junit.Assert.assertTrue(canRetryProblem(HostProblem.CameraFailed, HostLinkState.Disconnected, 10_000))
        org.junit.Assert.assertFalse(canRetryProblem(HostProblem.CameraBusy, HostLinkState.Disconnected, 9_999))
        org.junit.Assert.assertFalse(canRetryProblem(HostProblem.CameraBusy, HostLinkState.Connected, 30_000))
        for (problem in listOf(HostProblem.CameraBlocked, HostProblem.TooHot, HostProblem.NoCompatibleMode))
            org.junit.Assert.assertFalse(canRetryProblem(problem, HostLinkState.Disconnected, 30_000))
    }

    @Test fun problemCodesAreStableForHostBackoff() {
        assertEquals(listOf("busy", "blocked", "noMode", "failed", "hot", "failed"),
            HostProblem.entries.map { it.wireCode })
        assertEquals("sessionBusy", SESSION_BUSY_CODE)
        // Status-only answer while paused; hosts treat unknown codes as "not ready".
        assertEquals("paused", PAUSED_CODE)
        assertEquals("okuflow_camera", SOCKET_NAME)
        assertEquals("com.okuflow.camera.extra.HOST_LAUNCH", HOST_LAUNCH_EXTRA)
    }
    @Test fun pausedListenerKeepsTryingWhilePauseIsSaved() {
        // D3: the service may hold the socket name for a while; the bind is
        // retried for as long as Pause stays saved, not 20 times.
        var available = false
        var binds = 0
        var closes = 0
        val supervisor = PausedBindSupervisor {
            binds++
            if (!available) throw java.io.IOException("Address in use")
            AutoCloseable { closes++ }
        }
        repeat(100) { supervisor.tick(paused = true) }
        assertFalse(supervisor.bound)
        assertEquals(100, binds)
        available = true
        supervisor.tick(paused = true)
        assertTrue(supervisor.bound)
        supervisor.tick(paused = true)
        assertEquals(101, binds) // Kept, not rebound.
        supervisor.tick(paused = false)
        assertFalse(supervisor.bound)
        assertEquals(1, closes)
    }
    @Test fun resumeReleasesTheSocketBeforeTheServiceStarts() {
        // D3: closed synchronously before resume, never rebound during it,
        // bound again when the resume could not be saved.
        var open = 0
        val supervisor = PausedBindSupervisor { open++; AutoCloseable { open-- } }
        supervisor.tick(paused = true)
        assertEquals(1, open)
        val resumed = supervisor.releaseForResume {
            assertEquals(0, open)
            supervisor.tick(paused = true) // A supervision tick racing the resume.
            assertEquals(0, open)
            false // The resume could not be saved: still paused.
        }
        assertFalse(resumed)
        supervisor.tick(paused = true)
        assertEquals(1, open)
        assertTrue(supervisor.releaseForResume { true })
        supervisor.tick(paused = false)
        assertEquals(0, open)
    }
    @Test fun computerLaunchShowsOverTheLockScreenOnlyWhileNotPaused() {
        assertTrue(showOverKeyguardForHostLaunch(hostLaunch = true, paused = false))
        assertFalse(showOverKeyguardForHostLaunch(hostLaunch = true, paused = true))
        assertFalse(showOverKeyguardForHostLaunch(hostLaunch = false, paused = false))
        assertFalse(showOverKeyguardForHostLaunch(hostLaunch = false, paused = true))
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
