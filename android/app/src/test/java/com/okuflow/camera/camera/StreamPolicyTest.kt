package com.okuflow.camera.camera

import com.okuflow.camera.settings.CameraSettings
import org.junit.Assert.*
import org.junit.Test

class StreamPolicyTest {
    private val modes = listOf(StreamMode(3840, 2160, 30, 32_000_000), StreamMode(1920, 1080, 60, 18_000_000),
        StreamMode(1920, 1080, 30, 9_000_000), StreamMode(1280, 720, 30, 4_000_000))
    @Test fun automaticPrioritizesDetail() { assertEquals(3840, chooseMode(modes, 0, 60).width) }
    @Test fun ceilingAllowsFast1080() { assertEquals(60, chooseMode(modes, 1080, 60).fps) }
    @Test fun selects1440At60WhenSupportedOtherwiseFallsBack() {
        val qhd = StreamMode(2560, 1440, 60, 32_000_000)
        assertEquals(qhd, chooseMode(modes + qhd, 1440, 60))
        assertEquals(StreamMode(1920, 1080, 60, 18_000_000), chooseMode(modes, 1440, 60))
    }
    @Test fun thermalCeilingSelects30() { assertEquals(30, chooseMode(modes, 1080, 30).fps) }
    @Test fun everydayPickerExcludesLowResolutionAndLowRateEvenAtHighResolution() {
        val extra = listOf(StreamMode(256, 144, 60, 4_000_000), StreamMode(1280, 720, 15, 4_000_000),
            StreamMode(4000, 3000, 15, 16_000_000), StreamMode(1920, 704, 30, 4_000_000))
        assertEquals(modes, everydayModes(modes + extra))
        // Filtering the UI must not remove an older phone's valid capture/fallback modes.
        assertEquals(extra[0], chooseMode(extra, 144, 60))
    }
    @Test fun everydayPickerIncludes720At24InEitherOrientationAndKeepsReportedPairs() {
        val boundary = listOf(StreamMode(1280, 720, 24, 4_000_000), StreamMode(720, 1280, 24, 4_000_000),
            StreamMode(2560, 1280, 25, 12_000_000))
        assertEquals(boundary, everydayModes(boundary))
        assertTrue(everydayModes(listOf(StreamMode(640, 480, 30, 4_000_000))).isEmpty())
    }
    @Test fun exactDimensionsDoNotChooseAnotherAspectRatioOrLargerWidth() {
        val wide = StreamMode(2560, 1080, 60, 24_000_000)
        assertEquals(modes[1], chooseMode(modes + wide, 0, 60, 1920, 1080))
        assertEquals(modes[2], chooseMode(modes + wide, 0, 30, 1920, 1080))
    }
    @Test fun absentExactModeFallsBackWithinBothDimensionsAndRate() {
        assertEquals(modes.last(), chooseMode(modes.filter { it.width != 1920 }, 0, 60, 1920, 1080))
    }
    @Test fun legacyCeilingMigratesToActualPairWithoutChangingImageControls() {
        val old = CameraSettings(resolution = 1440, torch = true, exposure = 2, autofocus = false)
        val available = modes + StreamMode(2560, 1280, 30, 16_000_000)
        assertEquals(old.copy(resolution = 0, modeWidth = 2560, modeHeight = 1280, fps = 30),
            old.withAvailableModes(available))
    }
    @Test fun legacyAutomaticSizeWithRateCeilingMigratesToPair() {
        assertEquals(CameraSettings(modeWidth = 3840, modeHeight = 2160, fps = 30),
            CameraSettings(fps = 30).withAvailableModes(modes))
    }
    @Test fun changedCameraReconcilesPairWhileAutomaticRemainsAutomatic() {
        val selected = CameraSettings(modeWidth = 1920, modeHeight = 1080, fps = 60)
        assertEquals(selected, selected.withAvailableModes(modes))
        assertEquals(selected.copy(modeWidth = 1280, modeHeight = 720, fps = 30),
            selected.withAvailableModes(listOf(modes.last())))
        assertEquals(CameraSettings(), CameraSettings().withAvailableModes(modes))
        assertEquals(selected, selected.withAvailableModes(emptyList()))
        assertEquals(CameraSettings(), selected.withAvailableModes(listOf(modes.first())))
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsUnsupportedSelection() { chooseMode(modes, 480, 24) }
    @Test fun annexBPreserved() {
        val bytes = byteArrayOf(0, 0, 0, 1, 101, 2)
        assertArrayEquals(bytes, annexB(bytes))
    }
    @Test fun avccConverted() {
        assertArrayEquals(byteArrayOf(0, 0, 0, 1, 101, 2, 0, 0, 0, 1, 65),
            annexB(byteArrayOf(0, 0, 0, 2, 101, 2, 0, 0, 0, 1, 65)))
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsBrokenNal() { annexB(byteArrayOf(0, 0, 0, 12, 1)) }
}
