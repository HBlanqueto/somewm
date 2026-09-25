/*
 * blur.h - backdrop blur for clients and layer surfaces (SceneFX 0.5)
 *
 * SceneFX 0.4's per-buffer backdrop-blur API is gone in 0.5. Backdrop blur is
 * now a scene-graph node (wlr_scene_blur) that blurs the layers rendered
 * behind it within its box, clipped by corner radii and an optional
 * transparency-mask source (the object's own content buffer, so transparent
 * pixels stay sharp). Global blur device parameters go through
 * wlr_scene_set_blur_data() once per compositor.
 *
 * The mask source, box and corner radii all come from the owning object; this
 * API only manages the node. The node is re-applied on every surface commit,
 * because wlroots detaches the mask link and resets buffer state then.
 */

#ifndef SOMEWM_BLUR_H
#define SOMEWM_BLUR_H

#include <stdbool.h>
#include <lua.h>
#include "scenefx_compat.h"

struct wlr_box;
struct wlr_scene;
struct wlr_scene_blur;
struct wlr_scene_buffer;
struct wlr_scene_tree;

/** Per-object backdrop-blur config. NULL on an object means "blur off". */
typedef struct blur_config_t {
	bool enabled;       /**< Backdrop blur on for this object */
	int corner_radius;  /**< Uniform radius override; 0 = object's corners */
	float alpha;        /**< Blur alpha 0.0..1.0 */
	float strength;     /**< Blur strength 0.0..1.0 */
} blur_config_t;

/** Scene node backing one object's backdrop blur. */
typedef struct blur_nodes_t {
	struct wlr_scene_blur *node; /**< Blur node (NULL = off) */
	blur_config_t config;        /**< Config the node was created for */
	float fade;                  /**< Fade multiplier, 1.0 = full */
} blur_nodes_t;

/** Parse a backdrop_blur value (boolean or config table). */
bool blur_config_from_lua(lua_State *L, int idx, blur_config_t *config);

/** Push a blur config; enabled without custom values pushes `true`. */
void blur_config_to_lua(lua_State *L, const blur_config_t *config);

/**
 * Create, refit or release the blur node for one object.
 *
 * `area` is the node box in the parent's coordinates, `radii` the object's
 * per-corner radii in rounded-config order (TL, TR, BL, BR) to round the
 * blur to, and `mask` the content buffer whose transparent pixels must stay
 * unblurred (NULL: no transparency masking, the whole box blurs). A
 * disabled config or a degenerate box releases the node.
 */
void blur_apply(struct wlr_scene_tree *parent, blur_nodes_t *blur,
	const blur_config_t *config, const struct wlr_box *area,
	const int radii[4], struct wlr_scene_buffer *mask);

/** Destroy the blur node (no-op when there is none). */
void blur_release(blur_nodes_t *blur);

/** Scale the node's alpha/strength by `fade` (0..1), for opacity fades. */
void blur_set_fade(blur_nodes_t *blur, float fade);

/** Sample the SceneFX cached optimized blur instead of re-blurring the live
 * backdrop (the optimized-blur path for static backdrops like wallpapers). */
void blur_set_only_bottom_layer(blur_nodes_t *blur, bool only_bottom);

/** Global blur device parameters; forwards to wlr_scene_set_blur_data. */
void blur_set_data(struct wlr_scene *scene, int num_passes, int radius,
	float noise, float brightness, float contrast, float saturation);

/** Apply the compiled-in blur device defaults to a freshly created scene. */
void blur_setup_defaults(struct wlr_scene *scene);

#endif /* SOMEWM_BLUR_H */