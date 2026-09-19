/*
 * scenefx_compat.h - Conditional include for the scene-graph header.
 *
 * With SceneFX enabled, use its extended scene API (shader-rounded corners,
 * shadows, blur, buffer opacity). Otherwise fall back to vanilla wlroots.
 * The two APIs are source-compatible: SceneFX extends without replacing, so
 * translation units that only use the shared surface can include this header
 * and compile unchanged either way.
 */

#ifndef SOMEWM_SCENEFX_COMPAT_H
#define SOMEWM_SCENEFX_COMPAT_H

#ifdef HAVE_SCENEFX
#include <scenefx/types/wlr_scene.h>
#else
#include <wlr/types/wlr_scene.h>
#endif

#endif /* SOMEWM_SCENEFX_COMPAT_H */
