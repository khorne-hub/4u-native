// Performance readout (perfstats.cpp, from the MHGU port): the frame rate, the frame time and, on a phone,
// its temperature, in a small plate at the top of the screen
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void perfstats_set(bool visible, float fps_cap, bool spanish);  // the settings (any thread)
float perfstats_fps_cap(void);          // the settings' FPS limit as perfstats_set last had it (any thread)
void perfstats_poll(void);              // host main thread, each loop pass (runtime/ui.c): the phone's temperature, every 2 s at most
bool perfstats_visible(void);           // game thread (runtime/overlay.cpp): it is to be drawn this frame
void perfstats_draw(float w, float h);  // game thread, inside the overlay's ImGui frame (display size in pixels)
// the plate as last drawn, in display pixels: x0, y0, x1, y1; false while it isn't shown (the layout editor frames it
// to move it: touch.cpp; the MHGU port's the same)
bool perfstats_rect(float r[4]);
// (touch.cpp) the plate, pw x ph display pixels, where the layout in use has it (moved in the layout editor, kept in
// the layout and its profile): its top left, on the screen whole; false (x0, y0 untouched) while it is at its own place
bool touch_fps_place(float w, float h, float pw, float ph, float *x0, float *y0);

#ifdef __cplusplus
}
#endif
