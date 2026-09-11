#include "GameBackend.h"
#include "MultiplayerLog.h"
#include "NetworkManager.h"
#include "NetworkSynchronizer.h"
#include <chrono>

GameBackend::GameBackend() {
}

GameBackend::~GameBackend() {
}


void GameBackend::enqueuePacket(std::shared_ptr<znet::Packet> packet) {
	std::lock_guard<std::mutex> lock(queueMutex);
	packetQueue.push_back(std::move(packet));
}

void GameBackend::runOnMainThread(std::function<void()> task) {
	std::lock_guard<std::mutex> lock(queueMutex);
	mainThreadTasks.push_back(std::move(task));
}

void GameBackend::setOnConnected(std::function<void()> cb) {
	onconnected = std::move(cb);
}

void GameBackend::setOnDisconnected(std::function<void()> cb) {
	ondisconnected = std::move(cb);
}

// znet raises its connection events on the network thread, so both of these
// have to hop to update() before they reach game or UI state. Without that the
// disconnect handler builds a canvas off the render thread, which allocates
// textures and buffers with no GL context bound.
void GameBackend::notifyConnected() {
	runOnMainThread([this]() {
		if (onconnected) onconnected();
	});
}

void GameBackend::notifyDisconnected() {
	runOnMainThread([this]() {
		// update()'s keepalive timeout reaches the same callback, and the game
		// only expects to hear about a disconnect once.
		if (disconnectNotified) return;
		disconnectNotified = true;
		if (ondisconnected) ondisconnected();
	});
}

void GameBackend::attachNode(uint32_t netid, std::shared_ptr<gNode> node, bool local) {
	if (!node) return;
	std::lock_guard<std::mutex> lock(nodesmutex);
	NetNode entry;
	entry.local = local;
	// Without this a remote node lerps in from the origin until its first state packet.
	entry.targetX = node->getPosX();
	entry.targetY = node->getPosY();
	entry.targetZ = node->getPosZ();
	entry.node = std::move(node);
	nodes[netid] = std::move(entry);
}

void GameBackend::detachNode(uint32_t netid) {
	std::lock_guard<std::mutex> lock(nodesmutex);
	nodes.erase(netid);
}

void GameBackend::setOnJoin(std::function<void(uint32_t)> cb) {
	onjoin = std::move(cb);
}

void GameBackend::setOnTeamChanged(std::function<void(uint32_t, uint8_t)> cb) {
	onteamchanged = std::move(cb);
}

void GameBackend::setOnLeave(std::function<void(uint32_t)> cb) {
	onleave = std::move(cb);
}

// Handled by main thread via gipNetworkBackend::update(deltaTime)
void GameBackend::onPacketReceived(std::shared_ptr<znet::Packet> packet) {
	// Any packet at all proves the peer is alive. Keying liveness on keepalives
	// alone meant a host busy loading a map - which blocks its main thread for
	// well over the timeout - looked dead to a client that had already finished
	// loading, and the client kicked itself the moment the match began.
	timeSinceLastKeepAlive = 0.0f;
	if (packet->id() == PACKET_KEEPALIVE) {
		return;
	}

	if (packet->id() == PACKET_NODE_LEAVE) {
		auto p = std::static_pointer_cast<NodeLeavePacket>(packet);
		if (onleave) onleave(p->netid);
		
		// Remove from lobby if present
		for (auto it = roomPlayers.begin(); it != roomPlayers.end(); ++it) {
			if (it->id == p->netid) {
				roomPlayers.erase(it);
				chatRateStamps.erase(p->netid);
				publishPlayerCount();
				broadcastLobbyState();
				break;
			}
		}
		return;
	}
	
	if (packet->id() == PACKET_NODE_FIRE) {
		auto p = std::static_pointer_cast<PlayerFirePacket>(packet);
		if (onplayerfired) onplayerfired(p->shooterId, p->gunType, p->originX, p->originY, p->originZ, p->dirX, p->dirY, p->dirZ);
		return;
	}

	if (packet->id() == PACKET_NODE_HIT) {
		auto p = std::static_pointer_cast<PlayerHitPacket>(packet);
		if (onplayerhit) onplayerhit(p->attackerId, p->victimId, p->damage);
		return;
	}

	if (packet->id() == PACKET_NODE_KILLED) {
		auto p = std::static_pointer_cast<PlayerKilledPacket>(packet);
		if (onplayerkilled) onplayerkilled(p->killerId, p->victimId);
		return;
	}

	if (packet->id() == PACKET_PLAYER_PING_SNAPSHOT) {
		auto p = std::static_pointer_cast<PlayerPingSnapshotPacket>(packet);
		std::lock_guard<std::mutex> lock(pingsmutex);
		remotePings.clear();
		for (size_t i = 0; i < p->playerIds.size(); i++) {
			remotePings[p->playerIds[i]] = static_cast<int>(p->playerPings[i]);
		}
		return;
	}

	if (packet->id() == PACKET_NODE_STATE) {
		auto ev = std::static_pointer_cast<NodeStatePacket>(packet);

		// If this node ID hasn't been seen before, fire onJoin so the user
		// can create a visual and attachNode for it.
		// onjoin calls back into attachNode, so nodesmutex must not be held
		// across it.
		bool known;
		{
			std::lock_guard<std::mutex> lock(nodesmutex);
			known = nodes.find(ev->netid) != nodes.end();
		}
		if (!known && onjoin) onjoin(ev->netid);

		bool teamchanged = false;
		{
			std::lock_guard<std::mutex> lock(nodesmutex);
			auto it = nodes.find(ev->netid);
			if (it == nodes.end()) return;

			// Set target position for remote nodes instead of snapping them instantly
			if (!it->second.local) {
				it->second.targetX = ev->x;
				it->second.targetY = ev->y;
				it->second.targetZ = ev->z;
				it->second.targetYaw = ev->yaw;
				it->second.targetAnimState = ev->animState;
				if (it->second.team != ev->team) {
					it->second.team = ev->team;
					teamchanged = true;
				}
			}
		}
		if (teamchanged && onteamchanged) onteamchanged(ev->netid, ev->team);
		return;
	}

	if (packet->id() == PACKET_CHAT_MESSAGE) {
		auto p = std::static_pointer_cast<ChatMessagePacket>(packet);
		// The input box only accepts printable ASCII, but a modified client is
		// not bound by it, so the same restriction is applied to anything that
		// arrives over the network before it reaches a screen.
		p->text.erase(std::remove_if(p->text.begin(), p->text.end(),
			[](unsigned char c) { return c < 32 || c > 126; }), p->text.end());
		if (p->text.empty()) return;
		if (p->text.size() > ChatManager::MAX_TEXT_LENGTH) p->text.resize(ChatManager::MAX_TEXT_LENGTH);

		if (isServer()) {
			// The host is the only peer that routes, so it is the only one that
			// has to validate. Name and team are read from roomPlayers, which is
			// why this runs here on the main thread and not in the handler.
			const RoomPlayerInfo* sender = nullptr;
			for (const auto& rp : roomPlayers) {
				if (rp.id == p->senderId) { sender = &rp; break; }
			}
			if (!sender) return;
			if (!allowChatRate(p->senderId)) return;
			if (p->channel == CHAT_PRIVATE) {
				bool targetExists = false;
				for (const auto& rp : roomPlayers) {
					if (rp.id == p->targetId) { targetExists = true; break; }
				}
				if (!targetExists) return;
			}
			p->senderName = sender->name;
			relayChat(p);
		}

		if (shouldDisplayChat(p)) {
			ChatManager::getInstance()->receive(p->channel, p->senderId, p->senderName, p->text);
		}
		return;
	}

	if (packet->id() == PACKET_LOBBY_STATE) {
		auto p = std::static_pointer_cast<LobbyStatePacket>(packet);
		roomPlayers.clear();
		for (size_t i = 0; i < p->playerIds.size(); i++) {
			roomPlayers.push_back({p->playerIds[i], p->playerNames[i], p->playerTeams[i], p->playerReadys[i] != 0});
		}
		publishPlayerCount();
		NetworkManager::getInstance()->currentRoomCode = p->roomCode;
		if (onLobbyStateUpdated) onLobbyStateUpdated(p);
		return;
	}

	if (packet->id() == PACKET_START_MATCH) {
		matchInProgress = true;
		// The host is exempt from the ready-gate and never toggles its own
		// isReady, so without this its lobby-list entry would show "Not
		// Ready"/"In Lobby" forever even while the match is running.
		if (isServer()) {
			for (auto& rp : roomPlayers) rp.isReady = true;
			broadcastLobbyState();
		}
		if (onMatchStarted) onMatchStarted();
		return;
	}

	if (packet->id() == PACKET_LOBBY_KICK) {
		auto p = std::static_pointer_cast<LobbyKickPacket>(packet);
		if (onKicked) onKicked(p->reason);
		return;
	}

	// The following packets are ONLY handled by the Host.
	if (packet->id() == PACKET_LOBBY_JOIN) {
		auto p = std::static_pointer_cast<LobbyJoinPacket>(packet);
		MP_LOG_INFO("[GameBackend] Processing LOBBY_JOIN for ID: " << p->senderId << " Name: " << p->playerName);
		
		// A join is resent until the lobby lists the sender, so the same one
		// arriving twice has to mean the same player, not a second copy.
		for (const auto& rp : roomPlayers) {
			if (rp.id == p->senderId) {
				broadcastLobbyState();
				return;
			}
		}

		size_t maxSize = static_cast<size_t>(NetworkManager::getInstance()->getLobbyTeamSize()) * 2;
		if (roomPlayers.size() >= maxSize) {
			MP_LOG_ERROR("[GameBackend] Rejecting join, room is full!");
			return; // Reject join if room is full
		}
		
		// Auto-balance team
		int redCount = 0; int blueCount = 0;
		for (auto& rp : roomPlayers) { if (rp.team == 1) redCount++; else if (rp.team == 2) blueCount++; }
		uint8_t team = (redCount <= blueCount) ? 1 : 2;
		
		// Resolve duplicate names
		std::string finalName = p->playerName;
		int duplicateCount = 1;
		auto nameExists = [&](const std::string& n) {
			for (const auto& rp : roomPlayers) {
				if (rp.name == n) return true;
			}
			return false;
		};
		while (nameExists(finalName)) {
			finalName = p->playerName + " " + std::to_string(duplicateCount);
			duplicateCount++;
		}
		
		MP_LOG_INFO("[GameBackend] Added player " << p->senderId << " to roomPlayers as " << finalName);
		roomPlayers.push_back({p->senderId, finalName, team, false});
		publishPlayerCount();
		broadcastLobbyState();
		return;
	}

	if (packet->id() == PACKET_TOGGLE_READY) {
		auto p = std::static_pointer_cast<ToggleReadyPacket>(packet);
		for (auto& rp : roomPlayers) {
			if (rp.id == p->senderId) {
				rp.isReady = !rp.isReady;
				break;
			}
		}
		broadcastLobbyState();
		return;
	}

	if (packet->id() == PACKET_SWITCH_TEAM) {
		auto p = std::static_pointer_cast<SwitchTeamPacket>(packet);
		for (auto& rp : roomPlayers) {
			if (rp.id == p->senderId) {
				rp.team = p->teamId;
				break;
			}
		}
		broadcastLobbyState();
		return;
	}
}



// The host sees every message before routing it, including ones meant for the
// other team or for two other players. Without this it would display them all.
bool GameBackend::shouldDisplayChat(const std::shared_ptr<ChatMessagePacket>& p) const {
	if (!isServer()) return true;
	uint32_t localId = NetworkSynchronizer::getInstance()->getLocalNodeId();
	if (p->channel == CHAT_ALL) return true;
	if (p->channel == CHAT_TEAM) {
		// localTeam only tracks in-match team assignment; the lobby's team
		// switch updates roomPlayers alone. Resolving both sides from
		// roomPlayers is the only way this is correct in the lobby, and it also
		// makes a dedicated server (on no team, absent from roomPlayers) fall
		// through to false instead of matching everyone by localTeam's default.
		uint8_t senderTeam = 0, myTeam = 0;
		bool sfound = false, mfound = false;
		for (const auto& rp : roomPlayers) {
			if (rp.id == p->senderId) { senderTeam = rp.team; sfound = true; }
			if (rp.id == localId)     { myTeam = rp.team;     mfound = true; }
		}
		return sfound && mfound && senderTeam == myTeam;
	}
	if (p->channel == CHAT_PRIVATE) return p->targetId == localId || p->senderId == localId;
	return false;
}

// Three messages a second per player. Enough for conversation, not enough to
// flood every other client off the server.
bool GameBackend::allowChatRate(uint32_t senderId) {
	using clock = std::chrono::steady_clock;
	float now = std::chrono::duration<float>(clock::now().time_since_epoch()).count();
	auto& stamps = chatRateStamps[senderId];
	stamps.erase(std::remove_if(stamps.begin(), stamps.end(),
		[now](float t) { return now - t > 1.0f; }), stamps.end());
	if (stamps.size() >= 3) return false;
	stamps.push_back(now);
	return true;
}

void GameBackend::update(float deltaTime) {
	std::vector<std::shared_ptr<znet::Packet>> batch;
	std::vector<std::function<void()>> tasks;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		batch.swap(packetQueue);
		tasks.swap(mainThreadTasks);
	}

	for (const auto& p : batch) {
		onPacketReceived(p);
	}
	for (const auto& task : tasks) {
		task();
	}

	// A level load blocks the main thread for seconds, and the whole stall
	// arrives as one huge deltaTime. Counting that as network silence tripped
	// the timeout below on a host that was perfectly alive, which latched
	// disconnectNotified at match start and left the client deaf for the rest
	// of the session. A stalled frame resets the clocks instead of advancing
	// them; no packet could have arrived while the thread was blocked anyway.
	static constexpr float STALLED_FRAME_SECONDS = 0.25f;
	if (deltaTime > STALLED_FRAME_SECONDS) {
		keepAliveTimer = 0.0f;
		timeSinceLastKeepAlive = 0.0f;
		pingTimer = 0.0f;
		return;
	}

	// Both directions send them: the client to time out a silent host, the
	// host to keep every NAT mapping open.
	if (!disconnectNotified) {
		keepAliveTimer += deltaTime;
		if (keepAliveTimer > 1.0f) {
			keepAliveTimer = 0.0f;
			sendPacket(std::make_shared<KeepAlivePacket>());
		}
	}

	if (!isServer()) {
		timeSinceLastKeepAlive += deltaTime;
		// Comfortably past the worst honest stall, and still under znet's own
		// 10s session timeout so the transport is not the last to notice.
		if (timeSinceLastKeepAlive > 8.0f && !disconnectNotified) {
			disconnectNotified = true;
			if (ondisconnected) ondisconnected();
			return;
		}

		if (!disconnectNotified) {
			pingTimer += deltaTime;
			if (pingTimer >= 1.0f) {
				pingTimer = 0.0f;
				uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
				auto ping = std::make_shared<PingPacket>();
				ping->timestamp = now;
				ping->reportedPing = static_cast<uint32_t>(std::max(0, currentPing.load(std::memory_order_relaxed)));
				sendPacket(ping);
			}
		}
	} else {
		currentPing.store(0, std::memory_order_relaxed);
	}

	// Ease remote nodes toward the last position we heard about
	{
		std::lock_guard<std::mutex> lock(nodesmutex);
		for (auto& kv : nodes) {
			if (!kv.second.local) {
				float curX = kv.second.node->getPosX();
				float curY = kv.second.node->getPosY();
				float curZ = kv.second.node->getPosZ();

				// Simple Lerp: start + (end - start) * factor
				float lerpFactor = 15.0f * deltaTime;
				if (lerpFactor > 1.0f) lerpFactor = 1.0f;

				kv.second.node->setPosition(
					curX + (kv.second.targetX - curX) * lerpFactor,
					curY + (kv.second.targetY - curY) * lerpFactor,
					curZ + (kv.second.targetZ - curZ) * lerpFactor
				);
			}
		}
	}

	// Send each local node's position, throttled to a fixed network tick rate.
	// Collected under the lock and sent outside it, so a send never blocks a
	// reader on another thread.
	networkTimer += deltaTime;
	if (networkTimer >= 0.05f) {
		networkTimer = 0.f;

		struct LocalNodeState {
			uint32_t netid;
			float x, y, z, yaw;
			uint8_t animState;
		};
		std::vector<LocalNodeState> locals;
		{
			std::lock_guard<std::mutex> lock(nodesmutex);
			for (auto& kv : nodes) {
				if (!kv.second.local) continue;
				locals.push_back({kv.first, kv.second.node->getPosX(), kv.second.node->getPosY(),
				                  kv.second.node->getPosZ(), kv.second.localYaw, kv.second.localAnimState});
			}
		}

		for (const auto& local : locals) {
			broadcastState(local.netid, local.x, local.y, local.z, local.yaw, localTeam, local.animState);
		}
	}
}

void GameBackend::setLocalYaw(uint32_t netId, float yaw) {
	std::lock_guard<std::mutex> lock(nodesmutex);
	auto it = nodes.find(netId);
	if (it != nodes.end()) it->second.localYaw = yaw;
}

float GameBackend::getRemoteYaw(uint32_t netId) {
	std::lock_guard<std::mutex> lock(nodesmutex);
	auto it = nodes.find(netId);
	return it != nodes.end() ? it->second.targetYaw : 0.0f;
}

void GameBackend::setLocalAnimState(uint32_t netId, uint8_t animState) {
	std::lock_guard<std::mutex> lock(nodesmutex);
	auto it = nodes.find(netId);
	if (it != nodes.end()) it->second.localAnimState = animState;
}

uint8_t GameBackend::getRemoteAnimState(uint32_t netId) {
	std::lock_guard<std::mutex> lock(nodesmutex);
	auto it = nodes.find(netId);
	return it != nodes.end() ? it->second.targetAnimState : 0;
}

void GameBackend::sendFireEvent(uint32_t shooterId, uint8_t gunType, float ox, float oy, float oz, float dx, float dy, float dz) {
	broadcastFireEvent(shooterId, gunType, ox, oy, oz, dx, dy, dz);
}

void GameBackend::sendHitEvent(uint32_t attackerId, uint32_t victimId, float damage) {
	broadcastHitEvent(attackerId, victimId, damage);
}

void GameBackend::sendKillEvent(uint32_t killerId, uint32_t victimId) {
	broadcastKillEvent(killerId, victimId);
}

void GameBackend::setOnPlayerFired(std::function<void(uint32_t, uint8_t, float, float, float, float, float, float)> cb) {
	onplayerfired = std::move(cb);
}

void GameBackend::setOnPlayerHit(std::function<void(uint32_t, uint32_t, float)> cb) {
	onplayerhit = std::move(cb);
}

void GameBackend::setOnPlayerKilled(std::function<void(uint32_t, uint32_t)> cb) {
	onplayerkilled = std::move(cb);
}

void GameBackend::onPongReceived(uint64_t timestamp) {
	uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	int rtt = (now >= timestamp) ? static_cast<int>(now - timestamp) : 0;
	currentPing.store(rtt, std::memory_order_relaxed);
}

std::unordered_map<uint32_t, int> GameBackend::getRemotePings() const {
	std::lock_guard<std::mutex> lock(pingsmutex);
	return remotePings;
}
