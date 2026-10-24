package com.okuflow.camera.camera

import kotlin.math.abs
import kotlin.math.sqrt

/** Optical metadata describes a field of view, never a promise of lossless zoom. */
internal fun equivalentFocalLength(focalMm: Float?, sensorWidthMm: Float?, sensorHeightMm: Float?): Float? {
    val values = listOf(focalMm, sensorWidthMm, sensorHeightMm)
    if (values.any { it == null || !it.isFinite() || it <= 0f }) return null
    return (focalMm!! * 43.2666f / sqrt(sensorWidthMm!! * sensorWidthMm + sensorHeightMm!! * sensorHeightMm))
        .takeIf { it.isFinite() && it > 0f }
}

internal fun lensRole(front: Boolean, equivalentMm: Float?): String = when {
    front -> "front"
    equivalentMm == null -> "back"
    equivalentMm < 20f -> "ultrawide"
    equivalentMm >= 45f -> "telephoto"
    else -> "main"
}

/** Prefer an OEM logical rear camera; no ordering/ID assumptions about the phone. */
internal fun automaticCamera(cameras: List<CameraChoice>): CameraChoice? =
    cameras.filter { it.facing == 1 && it.physicalId == null }.minWithOrNull(
        compareBy<CameraChoice> { !it.logical }.thenBy { abs((it.equivalentMm ?: 26f) - 26f) }.thenBy { it.id })
        ?: cameras.firstOrNull { it.physicalId == null }
