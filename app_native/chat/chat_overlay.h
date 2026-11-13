// The chat over the game (chat_overlay.cpp, Dear ImGui): the chat button (tabarong's bubble: a tap opens the chat, a drag
// moves it; the number of lines not read on it) and the last lines for a few seconds beside it, as the chat's "Lines over
// the game" says. The same file is in both ports; each says when its touch controls show, when its menu is open, and how
// the chat opens.
#pragma once
#include <functional>

namespace chat {

struct OverlayPort {
    std::function<bool()> controls_shown;  // the touch controls are on the screen (the button shows with them)
    std::function<bool()> menu_open;       // the port's menu is open (nothing of this then)
    std::function<void()> open_chat;       // a tap on the button: the menu's chat page
    std::function<void(const char *)> log;
};
void overlay_setup(const OverlayPort &port);
bool overlay_frame();                 // each frame: true while something is to be drawn (never while both chats are off)
void overlay_draw(float w, float h);  // then, over the touch controls (the foreground list)
// a finger that landed on no control (fractions of the screen): true when the button took it; its moves and its lift
bool bubble_down(float fx, float fy);
void bubble_finger(bool up, float fx, float fy);
void bubble_size(float s);  // 1: its own; .6 to 2
float bubble_scale();
void bubble_reset();        // its place and size as they come

}  // namespace chat
