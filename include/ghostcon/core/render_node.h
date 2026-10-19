#pragma once

#include <stdlib.h>

/* DRM render-node path for headless GBM+EGL rendering (no DRM master).
 *
 * Defaults to /dev/dri/renderD128. Override with GHOSTCON_DRM_RENDER_NODE to
 * point the render smoke test and the headless tools at a different node (a
 * second GPU, or a virtual one), rather than hardcoding the first node.
 *
 * To run with no GPU at all, back the node with a virtual DRM device (e.g. the
 * vkms module) and force Mesa's software rasterizer:
 *
 *     GALLIUM_DRIVER=llvmpipe GHOSTCON_DRM_RENDER_NODE=/dev/dri/renderD128 \
 *         ./test_render
 *
 * EGL still needs a node to exist; the software driver removes the need for real
 * rendering hardware behind it, not the node itself.
 */
static inline const char *ghostcon_render_node(void)
{
    const char *env = getenv("GHOSTCON_DRM_RENDER_NODE");
    return (env && env[0]) ? env : "/dev/dri/renderD128";
}
