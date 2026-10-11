package com.okuflow.camera.setup

import org.junit.Assert.assertEquals
import org.junit.Test

class OemFamilyTest {

    @Test
    fun mapsKnownManufacturersCaseInsensitively() {
        assertEquals(OemFamily.Samsung, OemFamily.fromManufacturer("samsung"))
        assertEquals(OemFamily.Samsung, OemFamily.fromManufacturer("SAMSUNG"))
        assertEquals(OemFamily.Xiaomi, OemFamily.fromManufacturer("Xiaomi"))
        assertEquals(OemFamily.Xiaomi, OemFamily.fromManufacturer("Redmi"))
        assertEquals(OemFamily.Xiaomi, OemFamily.fromManufacturer("POCO"))
        assertEquals(OemFamily.OnePlusOppo, OemFamily.fromManufacturer("OnePlus"))
        assertEquals(OemFamily.OnePlusOppo, OemFamily.fromManufacturer("OPPO"))
        assertEquals(OemFamily.OnePlusOppo, OemFamily.fromManufacturer("realme"))
    }

    @Test
    fun unknownOrMissingManufacturerUsesGenericHint() {
        assertEquals(OemFamily.Generic, OemFamily.fromManufacturer("Google"))
        assertEquals(OemFamily.Generic, OemFamily.fromManufacturer(""))
        assertEquals(OemFamily.Generic, OemFamily.fromManufacturer(null))
        assertEquals(OemFamily.Samsung, OemFamily.fromManufacturer("  samsung "))
    }
}
