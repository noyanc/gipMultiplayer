#include "chat/ChatManager.h"
#include "GameBackend.h"
#include "GamePackets.h"
#include "NetworkManager.h"
#include "NetworkSynchronizer.h"

ChatManager* ChatManager::getInstance() {
	static ChatManager instance;
	return &instance;
}

void ChatManager::send(uint8_t channel, const std::string& text, uint32_t targetId) {
	if (channel != CHAT_ALL && channel != CHAT_TEAM && channel != CHAT_PRIVATE) return;
	if (text.empty()) return;

	auto* nm = NetworkManager::getInstance();
	auto active = nm->getBackend();
	if (!active) return;

	auto p = std::make_shared<ChatMessagePacket>();
	p->senderId = NetworkSynchronizer::getInstance()->getLocalNodeId();
	p->targetId = (channel == CHAT_PRIVATE) ? targetId : 0;
	p->channel = channel;
	p->text = text.substr(0, MAX_TEXT_LENGTH);

	// Same split the lobby actions use: the host routes its own message through
	// its queue, a client hands it to the host.
	if (nm->isHost()) active->enqueuePacket(p);
	else active->sendPacket(p);
}

void ChatManager::addLocal(uint8_t channel, const std::string& text) {
	if (text.empty()) return;
	ChatEntry e;
	e.channel = channel;
	e.senderId = 0;
	e.text = text.substr(0, MAX_TEXT_LENGTH);
	append(e);
}

void ChatManager::receive(uint8_t channel, uint32_t senderId, const std::string& senderName, const std::string& text) {
	if (isMutedText(senderId)) return;
	ChatEntry e;
	e.channel = channel;
	e.senderId = senderId;
	e.senderName = senderName;
	e.text = text;
	append(e);
}

void ChatManager::append(const ChatEntry& entry) {
	std::lock_guard<std::mutex> lk(chatmutex);
	history.push_back(entry);
	if (history.size() > MAX_HISTORY) {
		history.erase(history.begin(), history.begin() + (history.size() - MAX_HISTORY));
	}
}

std::vector<ChatEntry> ChatManager::getMessages() const {
	std::lock_guard<std::mutex> lk(chatmutex);
	return history;
}

void ChatManager::setMutedText(uint32_t playerId, bool mutedFlag) {
	std::lock_guard<std::mutex> lk(chatmutex);
	if (mutedFlag) muted.insert(playerId);
	else muted.erase(playerId);
}

bool ChatManager::isMutedText(uint32_t playerId) const {
	std::lock_guard<std::mutex> lk(chatmutex);
	return muted.find(playerId) != muted.end();
}

void ChatManager::update(float deltaTime) {
	std::lock_guard<std::mutex> lk(chatmutex);
	for (auto& e : history) e.age += deltaTime;
}

void ChatManager::clear() {
	std::lock_guard<std::mutex> lk(chatmutex);
	history.clear();
	// The mute list is deliberately kept: it is this player's preference, not
	// session state.
}
