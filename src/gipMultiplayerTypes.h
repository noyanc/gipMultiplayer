/*
 * gipMultiplayerTypes.h
 *
 * Plain data that crosses the plugin boundary. Everything the game is allowed
 * to see lives here or in NetworkManager's plain-type accessors. This header
 * must never include znet, GamePackets.h, or any engine header - that is the
 * whole point of it.
 */

#pragma once

#include <cstdint>
#include <string>

struct RoomPlayerInfo {
	uint32_t id = 0;
	std::string name;
	uint8_t team = 0;
	bool isReady = false;
};

// Wire channels are CHAT_ALL, CHAT_TEAM and CHAT_PRIVATE. CHAT_SYSTEM and
// CHAT_KILL are produced locally on each client from events it already
// receives, and are never serialized or sent.
enum ChatChannel : uint8_t {
	CHAT_ALL     = 0,
	CHAT_TEAM    = 1,
	CHAT_PRIVATE = 2,
	CHAT_SYSTEM  = 3,
	CHAT_KILL    = 4,
};
