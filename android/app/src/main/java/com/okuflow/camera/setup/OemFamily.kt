package com.okuflow.camera.setup

import java.util.Locale

/**
 * Where the "tap 7 times" entry lives differs by manufacturer. Only the
 * wording of the hint changes; the button always opens the system
 * "About phone" screen.
 */
enum class OemFamily {
    /** Pixel and most other phones: About phone > Build number. */
    Generic,

    /** About phone > Software information > Build number. */
    Samsung,

    /** Xiaomi, Redmi, POCO: About phone > OS version (MIUI version on older builds). */
    Xiaomi,

    /** OnePlus, OPPO, realme: About device > Version > Build number. */
    OnePlusOppo;

    companion object {
        fun fromManufacturer(manufacturer: String?): OemFamily =
            when (manufacturer?.trim()?.lowercase(Locale.ROOT)) {
                "samsung" -> Samsung
                "xiaomi", "redmi", "poco" -> Xiaomi
                "oneplus", "oppo", "realme" -> OnePlusOppo
                else -> Generic
            }
    }
}
