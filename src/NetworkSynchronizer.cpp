#include "NetworkSynchronizer.h"
#include "GameBackend.h"
#include <iostream>
#include <random>
#include <chrono>

static uint32_t MakeMultiplayerNodeId() {
    static std::mt19937 ridg((uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    static uint32_t cachedid = ridg();
    return cachedid;
}

NetworkSynchronizer::NetworkSynchronizer() {
    localmultiplayerboxid = MakeMultiplayerNodeId();
}

void NetworkSynchronizer::regenerateLocalNodeId() {
    std::mt19937 ridg((uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    localmultiplayerboxid = ridg();
}

NetworkSynchronizer* NetworkSynchronizer::getInstance() {
    static NetworkSynchronizer instance;
    return &instance;
}

void NetworkSynchronizer::setup() {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (!backend) return;

    // Boxes from an earlier match would otherwise be drawn for a lobby that
    // never joined them.
    {
        std::lock_guard<std::mutex> lock(playersmutex);
        remotemultiplayerboxes.clear();
        remoteteams.clear();
        playersnapshot.clear();
    }
    networktimer = 0.0f;

    localmultiplayerbox->setScale(0.12f);
    localmultiplayerbox->setPosition(1.5f, 0.25f, 1.5f);

    backend->attachNode(localmultiplayerboxid, localmultiplayerbox, true);
    
    uint8_t myTeam = 1;
    for (auto& rp : backend->roomPlayers) {
        if (rp.id == localmultiplayerboxid) {
            myTeam = rp.team;
            break;
        }
    }
    backend->setLocalTeam(myTeam);

    // Seed every other player already in the room with their actual starting
    // team. Without this, getRemoteTeam() falls back to team 1 for anyone who
    // hasn't switched teams since joining - which silently defeats the
    // friendly-fire check in generateBullet() whenever the local player is
    // also team 1.
    {
        std::lock_guard<std::mutex> lock(playersmutex);
        for (auto& rp : backend->roomPlayers) {
            if (rp.id != localmultiplayerboxid) remoteteams[rp.id] = rp.team;
        }
    }

    backend->setOnTeamChanged([this](uint32_t id, uint8_t teamId) {
        {
            std::lock_guard<std::mutex> lock(playersmutex);
            remoteteams[id] = teamId;
        }
        std::cout << "Multiplayer: Remote Player " << id << " switched to Team " << (int)teamId << std::endl;
    });

    backend->setOnJoin([this](uint32_t id) {
        auto backend = NetworkManager::getInstance()->getBackend();
        if (!backend) return;
        if (id == localmultiplayerboxid) return;

        auto box = std::make_shared<gNode>();
        box->setScale(0.12f);
        box->setPosition(1.5f, 0.25f, 1.5f);

        backend->attachNode(id, box, false);

        // Same fallback-to-team-1 problem as above: onJoin only carries an
        // id, so without this the newly joined player's team is unknown
        // until they explicitly switch.
        uint8_t joinedTeam = 1;
        for (auto& rp : backend->roomPlayers) {
            if (rp.id == id) {
                joinedTeam = rp.team;
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(playersmutex);
            remotemultiplayerboxes[id] = std::move(box);
            remoteteams[id] = joinedTeam;
        }
    });

    backend->setOnLeave([this](uint32_t id) {
        auto backend = NetworkManager::getInstance()->getBackend();
        if (backend) backend->detachNode(id);
        std::lock_guard<std::mutex> lock(playersmutex);
        remotemultiplayerboxes.erase(id);
        remoteteams.erase(id);
    });

    backend->setOnPlayerFired([this](uint32_t shooterId, uint8_t gunType, float ox, float oy, float oz, float dx, float dy, float dz) {
        if (onRemoteFire) onRemoteFire(shooterId, gunType, ox, oy, oz, dx, dy, dz);
      });

    backend->setOnPlayerHit([this](uint32_t attackerId, uint32_t victimId, float damage) {
         if (onRemoteHit) onRemoteHit(attackerId, victimId, damage);
      });

    backend->setOnPlayerKilled([this](uint32_t killerId, uint32_t victimId) {
        if (onRemoteKilled) onRemoteKilled(killerId, victimId);
    });
}

void NetworkSynchronizer::update(float deltaTime, float cameraX, float cameraY, float cameraZ, float cameraYaw, uint8_t animState) {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (!backend) return;

    networktimer += deltaTime;
    if (networktimer >= 0.05f) {
        networktimer = 0.0f;
        localmultiplayerbox->setPosition(cameraX, cameraY - 0.175f, cameraZ);
        backend->setLocalYaw(localmultiplayerboxid, cameraYaw);
        backend->setLocalAnimState(localmultiplayerboxid, animState);
    }

    publishSnapshot(backend);
}

// Rebuilt once per update() so readers on other threads never walk the live
// containers. Built in two locked steps rather than one, so the backend is
// never queried while playersmutex is held.
void NetworkSynchronizer::publishSnapshot(const std::shared_ptr<GameBackend>& backend) {
    std::vector<RemotePlayerState> next;
    {
        std::lock_guard<std::mutex> lock(playersmutex);
        next.reserve(remotemultiplayerboxes.size());
        for (const auto& kv : remotemultiplayerboxes) {
            if (!kv.second) continue;
            RemotePlayerState state;
            state.id = kv.first;
            state.x = kv.second->getPosX();
            state.y = kv.second->getPosY();
            state.z = kv.second->getPosZ();
            auto team = remoteteams.find(kv.first);
            state.team = team != remoteteams.end() ? team->second : 1;
            next.push_back(state);
        }
    }

    for (auto& state : next) {
        state.yaw = backend->getRemoteYaw(state.id);
        state.animState = backend->getRemoteAnimState(state.id);
    }

    std::lock_guard<std::mutex> lock(playersmutex);
    playersnapshot.swap(next);
}

std::vector<NetworkSynchronizer::RemotePlayerState> NetworkSynchronizer::getRemotePlayerStates() const {
    std::lock_guard<std::mutex> lock(playersmutex);
    return playersnapshot;
}

uint8_t NetworkSynchronizer::getRemoteTeam(uint32_t id) const {
    std::lock_guard<std::mutex> lock(playersmutex);
    auto it = remoteteams.find(id);
    return it != remoteteams.end() ? it->second : 1;
}

std::unordered_map<uint32_t, int> NetworkSynchronizer::getRemotePings() const {
    auto backend = NetworkManager::getInstance()->getBackend();
    return backend ? backend->getRemotePings() : std::unordered_map<uint32_t, int>();
}



void NetworkSynchronizer::switchTeam() {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (backend) {
        uint8_t newTeam = backend->getLocalTeam() == 1 ? 2 : 1;
        backend->setLocalTeam(newTeam);
        // roomPlayers is what the room list and team message routing read, and
        // it only follows a SwitchTeamPacket. Setting localTeam alone left the
        // two disagreeing after a switch made during a match: the player moved
        // team for position and damage purposes while their team messages kept
        // going to the team they had just left.
        NetworkManager::getInstance()->switchTeam(newTeam);
    }
}

void NetworkSynchronizer::cleanup() {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (!backend) return;

    backend->detachNode(localmultiplayerboxid);

    // Ids are collected first so detachNode, which takes the backend's own
    // lock, is never called while playersmutex is held.
    std::vector<uint32_t> detachids;
    {
        std::lock_guard<std::mutex> lock(playersmutex);
        detachids.reserve(remotemultiplayerboxes.size());
        for (const auto& kv : remotemultiplayerboxes) detachids.push_back(kv.first);
        remotemultiplayerboxes.clear();
        remoteteams.clear();
        playersnapshot.clear();
    }

    for (uint32_t id : detachids) backend->detachNode(id);
}

void NetworkSynchronizer::sendFireEvent(uint8_t gunType, float ox, float oy, float oz, float dx, float dy, float dz) {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (backend) {
        backend->sendFireEvent(localmultiplayerboxid, gunType, ox, oy, oz, dx, dy, dz);
    }
}

void NetworkSynchronizer::sendHitEvent(uint32_t victimId, float damage) {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (backend) {
        backend->sendHitEvent(localmultiplayerboxid, victimId, damage);
    }
}

void NetworkSynchronizer::sendKillEvent(uint32_t killerId, uint32_t victimId) {
    auto backend = NetworkManager::getInstance()->getBackend();
    if (backend) {
        backend->sendKillEvent(killerId, victimId);
    }
}

//Set Callbacks
void NetworkSynchronizer::setOnRemoteFire(std::function<void(uint32_t, uint8_t, float, float, float, float, float, float)> cb) {
    onRemoteFire = std::move(cb);
}

void NetworkSynchronizer::setOnRemoteHit(std::function<void(uint32_t, uint32_t, float)> cb) {
    onRemoteHit = std::move(cb);
}

void NetworkSynchronizer::setOnRemoteKilled(std::function<void(uint32_t, uint32_t)> cb) {
    onRemoteKilled = std::move(cb);
}
