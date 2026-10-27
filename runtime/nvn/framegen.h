// Frame generation (Video setting "frame_generation"): a game that presents at 30 fps (Animal Crossing: present
// interval 2, its world steps once per frame) is shown at 60 or 120 by inserting images between its frames. The game
// keeps its own pace, so its speed stays right; the window shows each game frame one in-between later. The images come
// from MetalFX's frame interpolator at 60 where it's available (frame_generation_method = metalfx, the default: with
// the game's depth and the motion vectors and HUD layer of motion.h), else from an optical flow between the frames.
#pragma once
#import <Metal/Metal.h>

// Images shown per game frame (1: off - the setting is off, the game already presents at the target rate, or it runs
// uncapped). The target is capped at the display's refresh rate (120 on a 60 Hz display: 60). interval: the game's
// present interval (1: 60 fps, 2: 30).
int framegen_factor(int interval);
// Keeps `cur` (the game's new image) and, when the previous one fits, encodes the optical flow between them and the
// n - 1 in-between images into cb. False: nothing to show between (first frame, size / format change).
bool framegen_prepare(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> cur, int n);
// The in-between image k of n (1 <= k < n), at time k / n from the previous frame to `cur`.
id<MTLTexture> framegen_image(int k);
void framegen_reset(void);  // (a reboot / the game's textures gone: no history)
bool framegen_wants_motion(void);  // (motion.h: MetalFX would make this frame's in-between image)
