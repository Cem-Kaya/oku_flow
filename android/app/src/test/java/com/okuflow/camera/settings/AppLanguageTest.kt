package com.okuflow.camera.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Locale

class AppLanguageTest {
    @Test fun supportedTagsAreCompleteAndCanonical() {
        assertEquals(25, AppLanguage.entries.size)
        assertEquals(25, AppLanguage.entries.map { it.tag }.toSet().size)
        AppLanguage.entries.forEach {
            assertTrue(it.nativeName.isNotBlank())
            assertEquals(it, AppLanguage.fromTag(it.tag))
        }
        assertEquals(AppLanguage.PortugueseBrazil, AppLanguage.fromTag("PT_br"))
        assertNull(AppLanguage.fromTag(null))
        assertNull(AppLanguage.fromTag("system"))
        assertNull(AppLanguage.fromTag("unknown"))
    }

    @Test fun localeMatchingPreservesChineseScriptAndLegacyTags() {
        assertEquals(AppLanguage.Dutch, AppLanguage.forLocale(Locale.forLanguageTag("nl-BE")))
        assertEquals(AppLanguage.PortugueseBrazil, AppLanguage.forLocale(Locale.forLanguageTag("pt-PT")))
        assertEquals(AppLanguage.ChineseSimplified, AppLanguage.forLocale(Locale.forLanguageTag("zh-CN")))
        assertEquals(AppLanguage.ChineseSimplified, AppLanguage.forLocale(Locale.forLanguageTag("zh-Hans-TW")))
        assertNull(AppLanguage.forLocale(Locale.forLanguageTag("zh-Hant")))
        assertNull(AppLanguage.forLocale(Locale.forLanguageTag("zh-TW")))
        assertEquals(AppLanguage.Hebrew, AppLanguage.forLocale(Locale.forLanguageTag("iw-IL")))
        assertEquals(AppLanguage.Indonesian, AppLanguage.forLocale(Locale.forLanguageTag("in-ID")))
        assertNull(AppLanguage.forLocale(Locale.forLanguageTag("zz-ZZ")))
    }
}
