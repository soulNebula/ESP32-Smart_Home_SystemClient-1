/*
 * 模块：
 *   Gradle 的总设置。插件和依赖从哪儿下、仓库怎么配，都在这一个文件里定。
 *   下面的 app 模块（android/app/build.gradle.kts）和根工程（android/build.gradle.kts）
 *   都按这儿的规矩找东西；干活用 android/build.ps1。
 *
 * 功能：
 *   定插件从哪下
 *   定依赖仓库
 *   给工程起名
 *   挂上 app 模块
 */
pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

// 功能：工程名叫 SmartHomeBLE
rootProject.name = "SmartHomeBLE"
// 功能：带上 app 这个模块
include(":app")
