/*
 * ChatManager.h
 *
 * Text chat: history, mute list, and the send path. Generic on purpose - it
 * knows about channels and player ids, and nothing about lobbies, matches or
 * kills. Formatting and presentation belong to the game.
 *
 * This header must stay free of znet and GamePackets.h. Only ChatManager.cpp
 * sees a packet.
 */

#pragma once

#include "gipMultiplayerTypes.h"
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

struct ChatEntry {
	uint8_t channel = CHAT_ALL;
	uint32_t senderId = 0;
	std::string senderName;
	std::string text;
	float age = 0.0f;  // seconds since arrival, for the fade in game
};

class ChatManager {
public:
	static ChatManager* getInstance();

	// Longest message the host will accept; the input box enforces the same
	// number so a truncation is never a surprise.
	static constexpr size_t MAX_TEXT_LENGTH = 200;
	static constexpr size_t MAX_HISTORY = 100;

	// Sends over the network. targetId is only read for CHAT_PRIVATE.
	// CHAT_SYSTEM and CHAT_KILL are rejected here - use addLocal for those.
	void send(uint8_t channel, const std::string& text, uint32_t targetId = 0);

	// Appends a line that was produced on this client and is not sent anywhere.
	void addLocal(uint8_t channel, const std::string& text);

	// Called by GameBackend when a chat packet has been routed to us.
	void receive(uint8_t channel, uint32_t senderId, const std::string& senderName, const std::string& text);

	// A copy, so the draw path never holds the lock.
	std::vector<ChatEntry> getMessages() const;

	void setMutedText(uint32_t playerId, bool muted);
	bool isMutedText(uint32_t playerId) const;

	void update(float deltaTime);
	void clear();

private:
	ChatManager() = default;
	~ChatManager() = default;

	void append(const ChatEntry& entry);

	// Guards history and muted. Never nested with playersmutex or nodesmutex.
	mutable std::mutex chatmutex;
	std::vector<ChatEntry> history;
	std::unordered_set<uint32_t> muted;
};
