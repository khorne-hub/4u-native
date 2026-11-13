// MH4U for Android's side of the port's chat (chat_app.cpp; the chat itself: chat/chat.h).
#pragma once
#include <string>

void chat_app_init();   // the interface's first frame: the chat started (its relays only if the player turned them on)
void chat_app_frame();  // each frame, the menu open or not: the chat's state, and over the game its button and last lines
// a line of the room's chat (net_rooms.cpp): someone's, or this phone's as it was sent
void chat_app_room_line(const std::string& nick, const std::string& text, bool mine);
