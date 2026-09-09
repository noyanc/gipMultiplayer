#pragma once

#include "NetworkManager.h"
#include "gNode.h"
#include <mutex>
#include <unordered_map>
#include <memory>
#include <vector>

class NetworkSynchronizer {
public:
    // A remote player as of the last update(), copied out under lock. Readers
    // on any thread get a consistent view that nothing else can mutate.
    struct RemotePlayerState {
        uint32_t id = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float yaw = 0.0f;
        uint8_t animState = 0;
        uint8_t team = 1;
    };

    static NetworkSynchronizer* getInstance();

    uint32_t getLocalNodeId() const { return localmultiplayerboxid; }
    void regenerateLocalNodeId();

    // Call this once when the game loads
    void setup();

    // Call this every frame to sync local player position and process packets
    void update(float deltaTime, float cameraX, float cameraY, float cameraZ, float cameraYaw, uint8_t animState = 0);

    // Clean up when the game ends
    void cleanup();

    // Example feature: switch teams
    void switchTeam();

    // Safe from any thread: returns a copy of the snapshot published by the
    // last update(). Prefer this over getRemotePlayers().
    std::vector<RemotePlayerState> getRemotePlayerStates() const;

    // Legacy accessor. Hands out a reference to the live map, which onJoin and
    // onLeave mutate, so it is only valid on the thread that drives update().
    const std::unordered_map<uint32_t, std::shared_ptr<gNode>>& getRemotePlayers() const { return remotemultiplayerboxes; }

    uint8_t getRemoteTeam(uint32_t id) const;

    // Every known player's ping in milliseconds, keyed by id. Safe from any
    // thread.
    std::unordered_map<uint32_t, int> getRemotePings() const;

    void sendFireEvent(uint8_t gunType, float ox, float oy, float oz, float dx, float dy, float dz);
    void sendHitEvent(uint32_t victimId, float damage);
    void sendKillEvent(uint32_t killerId, uint32_t victimId);

    void setOnRemoteFire(std::function<void(uint32_t, uint8_t, float, float, float, float, float, float)> cb);
    void setOnRemoteHit(std::function<void(uint32_t, uint32_t, float)> cb);
    void setOnRemoteKilled(std::function<void(uint32_t, uint32_t)> cb);


private:
    NetworkSynchronizer();
    ~NetworkSynchronizer() = default;

    void publishSnapshot(const std::shared_ptr<GameBackend>& backend);

    uint32_t localmultiplayerboxid = 0;
    std::shared_ptr<gNode> localmultiplayerbox = std::make_shared<gNode>();

    // playersmutex guards remotemultiplayerboxes, remoteteams and playersnapshot.
    // The gNode objects themselves are only ever touched by the thread that
    // drives update(): GameBackend lerps them, publishSnapshot() copies them out.
    // Readers on other threads see the snapshot, never the nodes.
    mutable std::mutex playersmutex;
    std::unordered_map<uint32_t, std::shared_ptr<gNode>> remotemultiplayerboxes;
    std::unordered_map<uint32_t, uint8_t> remoteteams;
    std::vector<RemotePlayerState> playersnapshot;
    float networktimer = 0.0f;

    std::function<void(uint32_t, uint8_t, float, float, float, float, float, float)> onRemoteFire;
    std::function<void(uint32_t, uint32_t, float)> onRemoteHit;
    std::function<void(uint32_t, uint32_t)> onRemoteKilled;
};
