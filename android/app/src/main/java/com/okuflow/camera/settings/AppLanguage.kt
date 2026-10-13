package com.okuflow.camera.settings

import android.app.LocaleManager
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.LocaleList
import android.content.res.Resources
import androidx.annotation.ChecksSdkIntAtLeast
import androidx.core.content.edit
import java.util.Locale

/**
 * OkuFlow Camera's 25 UI languages, matching OkuFlow for Windows. The order is
 * the Advanced settings picker order after "System default". Names are native
 * so someone stuck in an unfamiliar language can still find their own.
 */
enum class AppLanguage(val tag: String, val nativeName: String) {
    English("en", "English"),
    Turkish("tr", "Türkçe"),
    German("de", "Deutsch"),
    French("fr", "Français"),
    Spanish("es", "Español"),
    Italian("it", "Italiano"),
    PortugueseBrazil("pt-BR", "Português (Brasil)"),
    Russian("ru", "Русский"),
    Ukrainian("uk", "Українська"),
    Arabic("ar", "العربية"),
    Hindi("hi", "हिन्दी"),
    Japanese("ja", "日本語"),
    Korean("ko", "한국어"),
    ChineseSimplified("zh-Hans", "简体中文"),
    Dutch("nl", "Nederlands"),
    Polish("pl", "Polski"),
    Czech("cs", "Čeština"),
    Swedish("sv", "Svenska"),
    Indonesian("id", "Bahasa Indonesia"),
    Vietnamese("vi", "Tiếng Việt"),
    Thai("th", "ไทย"),
    Bengali("bn", "বাংলা"),
    Persian("fa", "فارسی"),
    Hebrew("he", "עברית"),
    Urdu("ur", "اردو");

    companion object {
        /** Exact supported tag (case-insensitive, '_' accepted for '-'), or null. */
        fun fromTag(tag: String?): AppLanguage? {
            val normalized = tag?.trim()?.replace('_', '-')?.lowercase() ?: return null
            return entries.firstOrNull { it.tag.lowercase() == normalized }
        }

        /**
         * The supported language Android would use for [locale]: every
         * Portuguese locale maps to Brazilian Portuguese, Traditional Chinese
         * does not map to Simplified Chinese, and legacy iw/in codes map to
         * Hebrew/Indonesian.
         */
        fun forLocale(locale: Locale): AppLanguage? {
            val language = when (locale.language) { "iw" -> "he"; "in" -> "id"; else -> locale.language }
            return when (language) {
                "pt" -> PortugueseBrazil
                "zh" -> if (isSimplifiedChinese(locale)) ChineseSimplified else null
                else -> entries.firstOrNull { it.tag == language }
            }
        }

        private fun isSimplifiedChinese(locale: Locale): Boolean = when (locale.script) {
            "Hans" -> true
            "Hant" -> false
            else -> locale.country !in setOf("TW", "HK", "MO")
        }
    }
}

/**
 * Persists the per-app language. An empty selection means "System default",
 * which keeps following later phone language changes.
 *
 * Android 13+ stores the choice in the framework per-app language service,
 * which also backs Settings → Apps → Language. Android 11–12 has no such
 * service, so the choice is kept in a private preference and applied by
 * wrapping each Activity/Service base context ([wrap]).
 */
object AppLanguageStore {
    private const val PREFERENCES = "language"
    private const val KEY_TAG = "tag"

    @ChecksSdkIntAtLeast(api = Build.VERSION_CODES.TIRAMISU)
    private fun usesFramework() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU

    /** The explicit app language, or null when following the phone. */
    fun selected(context: Context): AppLanguage? = if (usesFramework()) {
        val locales = context.getSystemService(LocaleManager::class.java).applicationLocales
        if (locales.isEmpty) null else AppLanguage.forLocale(locales[0])
    } else {
        AppLanguage.fromTag(preferences(context).getString(KEY_TAG, null))
    }

    /**
     * Saves [language] (null = System default). Android 13+ recreates visible
     * activities itself; on older versions the caller recreates the activity.
     * @return true when the caller must recreate the activity.
     */
    fun select(context: Context, language: AppLanguage?): Boolean {
        if (usesFramework()) {
            context.getSystemService(LocaleManager::class.java).applicationLocales =
                if (language == null) LocaleList.getEmptyLocaleList() else LocaleList.forLanguageTags(language.tag)
            return false
        }
        preferences(context).edit(commit = true) {
            if (language == null) remove(KEY_TAG) else putString(KEY_TAG, language.tag)
        }
        return true
    }

    /**
     * The language the phone resolves when following the system, used to label
     * "System default (…)". English is the unqualified resource fallback.
     */
    fun systemLanguage(): AppLanguage {
        // The process default follows an explicit app language; the system
        // resources keep the phone's own language list.
        val system = Resources.getSystem().configuration.locales
        for (index in 0 until system.size()) {
            AppLanguage.forLocale(system[index])?.let { return it }
        }
        return AppLanguage.English
    }

    /**
     * Applies a saved Android 11–12 choice to an Activity/Service base
     * context. Android 13+ and System default return [base] unchanged.
     */
    fun wrap(base: Context): Context {
        if (usesFramework()) return base
        val language = AppLanguage.fromTag(preferences(base).getString(KEY_TAG, null)) ?: return base
        val locale = Locale.forLanguageTag(language.tag)
        val configuration = Configuration(base.resources.configuration)
        configuration.setLocales(LocaleList(locale))
        configuration.setLayoutDirection(locale)
        return base.createConfigurationContext(configuration)
    }

    private fun preferences(context: Context) =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
}
