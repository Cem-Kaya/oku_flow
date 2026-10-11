package com.okuflow.camera.settings

import org.junit.Assert.*
import org.junit.Test

class CameraSettingsTest {
    @Test fun newConnectionsClearOnlyTheLight() {
        val settings = CameraSettings(cameraId = "0", modeWidth = 1920, modeHeight = 1080,
            fps = 60, codec = "hevc", torch = true, exposure = 2, autofocus = false)
        assertFalse(CameraSettings().torch)
        assertFalse(settings.forNewHostSession().torch)
        assertEquals(settings.copy(torch = false), settings.forNewHostSession())
    }
    @Test fun corruptPreferencesReturnSafeDefaults() {
        val settings = CameraSettings(resolution = -1, fps = 120, bitrateMbps = Int.MAX_VALUE,
            codec = "unknown", stabilization = "both", exposure = 99).sanitized()
        assertEquals(0, settings.resolution)
        assertEquals(60, settings.fps)
        assertEquals(0, settings.bitrateMbps)
        assertEquals("avc", settings.codec)
        assertEquals("optical", settings.stabilization)
        assertEquals(12, settings.exposure)
    }
    @Test fun newInstallationsAndResetPreferHevcButExplicitAvcIsKept() {
        assertEquals("hevc", CameraSettings().codec)
        assertEquals("hevc", CameraSettings().sanitized().codec)
        assertEquals("hevc", storedCodec(null))
        assertEquals("avc", CameraSettings(codec = storedCodec("avc")).sanitized().codec)
        assertEquals("hevc", CameraSettings(codec = storedCodec("hevc")).sanitized().codec)
    }
    @Test fun supportedPreferencesAndCameraIdentitySurvive() {
        val settings = CameraSettings(cameraId = "2", resolution = 1440, fps = 60, bitrateMbps = 40,
            codec = "hevc", stabilization = "electronic", torch = true, exposure = -2, autofocus = false)
        assertEquals(settings, settings.sanitized())
    }
    @Test fun discoveredPairAndUnusualReportedRateSurvive() {
        val settings = CameraSettings(modeWidth = 2560, modeHeight = 1280, fps = 25)
        assertTrue(settings.hasExplicitMode)
        assertEquals(settings, settings.sanitized())
    }
    @Test fun invalidOrPartialDimensionsResetTogether() {
        for (settings in listOf(CameraSettings(modeWidth = 1920), CameraSettings(modeHeight = 1080),
            CameraSettings(modeWidth = Int.MAX_VALUE, modeHeight = 1080))) {
            val clean = settings.sanitized()
            assertFalse(clean.hasExplicitMode)
            assertEquals(0, clean.modeWidth)
            assertEquals(0, clean.modeHeight)
        }
    }
}
