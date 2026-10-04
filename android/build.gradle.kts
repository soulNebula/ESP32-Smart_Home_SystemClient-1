/*
 * 模块：
 *   根工程的编译单。只管一件事：把安卓插件、Kotlin 插件和界面插件的版本号
 *   定死在这儿，具体用不用交给 app 模块（android/app/build.gradle.kts）。
 *   插件从哪儿下由 android/settings.gradle.kts 说了算。
 *
 * 功能：
 *   定三个插件版本
 *   都不在这里启用
 */
plugins {
    id("com.android.application") version "8.7.0" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.0.21" apply false
}
