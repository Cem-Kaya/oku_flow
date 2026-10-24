package com.okuflow.camera.camera

import org.junit.Assert.*
import org.junit.Test

class LensIdentityTest {
    @Test fun rolesUseFieldOfViewRatherThanOpaqueIdsOrFocalLengthAlone() {
        assertEquals("ultrawide", lensRole(false, equivalentFocalLength(2f, 6f, 4f)))
        assertEquals("main", lensRole(false, equivalentFocalLength(4.5f, 6f, 4f)))
        assertEquals("telephoto", lensRole(false, equivalentFocalLength(12f, 6f, 4f)))
        assertEquals("front", lensRole(true, 70f))
        assertEquals("back", lensRole(false, equivalentFocalLength(4f, 0f, 4f)))
        assertNull(equivalentFocalLength(Float.NaN, 6f, 4f))
        assertNull(equivalentFocalLength(null, 6f, 4f))
    }

    @Test fun automaticPrefersLogicalRearAndNeverAPhysicalChild() {
        val front = CameraChoice("0", 0, 0)
        val tele = CameraChoice("1", 1, 90, equivalentMm = 80f)
        val logical = CameraChoice("9", 1, 90, logical = true, equivalentMm = 26f)
        val child = CameraChoice("physical/9/2", 1, 90, "9", "2", equivalentMm = 26f)
        assertEquals(logical, automaticCamera(listOf(front, child, tele, logical)))
        assertEquals(tele, automaticCamera(listOf(front, child, tele)))
        assertEquals(front, automaticCamera(listOf(front)))
        assertNull(automaticCamera(emptyList()))
    }
}
