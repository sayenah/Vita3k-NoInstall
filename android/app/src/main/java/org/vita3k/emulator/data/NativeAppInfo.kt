package org.vita3k.emulator.data

data class NativeAppInfo(
    val titleId: String,
    val title: String,
    val category: String,
    val appVer: String,
    val iconPath: String,
    // Non-empty for a ROM entry (a game played from this archive with no install). Field order must
    // match the native NativeAppInfo ctor in native_apps.cpp.
    val archivePath: String,
    val hasCustomConfig: Boolean,
    val compatibility: Int,
    val lastPlayed: Long,
    val playtime: Long
)
