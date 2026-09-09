#pragma once

#include "gipMultiplayerTypes.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace znet {
    class Client;
    class Packet;
}

// Transport lives behind the facade; the game never names it.
class GameBackend;
class LobbyStatePacket;

class NetworkManager {
public:
    // Global access to the Network Manager
    static NetworkManager* getInstance();

    void disconnect();

    // Authentication & Token Management
    void loginUser(const std::string& email, const std::string& password);
    void loginWithToken(const std::string& email, const std::string& token);
    void registerUser(const std::string& username, const std::string& email, const std::string& password);
    void logoutUser();

    // Secure Session Management (AES-256 encrypted token storage)
    void saveSession(const std::string& email, const std::string& sessionToken);
    bool loadSession(std::string& outEmail, std::string& outSessionToken);
    void clearSession();
    bool hasSavedSession() const;
    void autoLogin();

    bool isHost() const { return hostMode; }

    // Lobby Actions
    void hostLobby(const std::string& playerName, const std::string& lobbyName, uint8_t teamSize, bool isPrivate = false, const std::string& password = "");
    void joinLobby(const std::string& ip, const std::string& playerName, const std::string& password = "", bool forceDirect = false);

    // Server browsing. Every result arrives through onServerQueried on the
    // main thread, whichever of the three started it.
    void clearQueries();
    void queryServer(const std::string& ip);
    void queryRoomCode(const std::string& roomCode);
    void refreshGlobalServers();

    uint8_t getLobbyTeamSize() const { return lobbyTeamSize; }
    void setLobbyTeamSize(uint8_t size) { lobbyTeamSize = size; }

    void toggleReady();
    void switchTeam(uint8_t teamId);
    void startMatch(); // Only works if isHost() is true
    void kickPlayer(uint32_t playerId);

    int getPing() const;

    // Voice Chat Controls & Modes
    enum VoiceChatMode {
        VOICE_MODE_OFF = 0,
        VOICE_MODE_TOGGLE = 1,
        VOICE_MODE_PUSH_TO_TALK = 2
    };

    void setVoiceMode(VoiceChatMode mode);
    VoiceChatMode getVoiceMode() const { return voiceMode; }

    void setProximityChatEnabled(bool enabled);
    bool isProximityChatEnabled() const { return proximityChatEnabled.load(std::memory_order_acquire); }
    float getProximityMaxDistance() const { return 12.0f; }
    float getProximityFullVolumeDistance() const { return 1.2f; }
    float calculateProximityVolume(float distance) const;

    void setHearEnemiesVoice(bool hear);
    bool canHearEnemiesVoice() const;

    void handleVoiceKeyDown();
    void handleVoiceKeyUp();

    void startVoiceTransmission();
    void stopVoiceTransmission();
    bool isVoiceTransmitting() const;
    bool isPlayerTalking(uint32_t playerId) const;
    void setPlayerVoiceMuted(uint32_t playerId, bool muted);
    void setPlayerVoiceVolume(uint32_t playerId, float volume);

    void setMicrophoneVolume(int volume);
    int getMicrophoneVolume() const;
    void setVoicePlaybackVolume(int volume);
    int getVoicePlaybackVolume() const;

    std::vector<std::string> getCaptureDeviceNames();
    int getCaptureDeviceIndex() const;
    void setCaptureDeviceIndex(int index);

    std::vector<std::string> getPlaybackDeviceNames();
    int getPlaybackDeviceIndex() const;
    void setPlaybackDeviceIndex(int index);

    std::string getPlayerName(uint32_t netId) const;

    // Plain-type facade. The game uses these instead of reaching for the
    // backend, so no transport type appears in game code.
    bool isConnected() const;
    uint8_t getLocalTeam() const;
    // A copy, safe to read from any thread. roomPlayers itself is main-thread
    // only, so handing out a reference would make that easy to violate.
    std::vector<RoomPlayerInfo> getRoomPlayers() const;
    // Enters a match that is already running, for global-server lobbies. This
    // is NOT startMatch(): there is no ready-gate and no host check, and it
    // only triggers the local match transition.
    void enterMatchInProgress();
    // Returns 0 when no player in the room carries that name. Case-sensitive,
    // matching how names are compared everywhere else in the lobby.
    uint32_t findPlayerIdByName(const std::string& name) const;

    // Callbacks for UI
    void setOnServerQueried(std::function<void(std::string, std::string, std::string, std::string, std::string, bool, bool, bool)> cb) { onServerQueried = cb; }
    // Lobby snapshot: the player list plus the two room-wide flags.
    void setOnLobbyStateUpdated(std::function<void(const std::vector<RoomPlayerInfo>&, bool isGlobalServer, bool matchInProgress)> cb) { onLobbyStateUpdated = cb; }
    void setOnMatchStarted(std::function<void()> cb) { onMatchStarted = cb; }
    // A disconnect can land while nothing is listening: the lobby canvas clears
    // its handler on the way out and the game canvas only registers its own in
    // setup(). Dropping it there left the client sitting in a match against a
    // server that was gone, so it is held and delivered to the next handler.
    void setOnDisconnected(std::function<void()> cb) {
        onDisconnected = std::move(cb);
        if (onDisconnected && disconnectPending) {
            disconnectPending = false;
            onDisconnected();
        }
    }
    void setOnKicked(std::function<void(std::string)> cb) { onKicked = cb; }

    std::function<void(std::string, std::string, std::string, std::string, std::string, bool, bool, bool)> onServerQueried;
    std::function<void(const std::vector<RoomPlayerInfo>&, bool, bool)> onLobbyStateUpdated;
    std::function<void()> onMatchStarted;
    std::function<void()> onDisconnected;
    std::function<void(std::string)> onKicked;

    // Installs a backend built elsewhere. The dedicated server entry point
    // uses this so its loop runs through update() like the game's does.
    void useBackend(std::shared_ptr<GameBackend> next);

    // Plugin-internal. Returns null when not connected. The game must use the
    // plain-type accessors instead; nothing in game_martyr may call this.
    std::shared_ptr<GameBackend> getBackend() const;

    // Call this from the game's main update loop to process network events
    void update(float deltaTime);

    std::string lobbyName;
    std::string currentRoomCode = "";

    // Auth State. The request runs on its own thread, so these go through
    // accessors rather than being touched directly.
    enum AuthStatus { AUTH_NONE, AUTH_PENDING, AUTH_SUCCESS, AUTH_FAIL };
    AuthStatus authStatus() const { return currentAuthStatus.load(std::memory_order_acquire); }
    std::string authMessage() const;
    std::string loggedInUsername() const;
    void clearAuthStatus();

    // Called by the packet handlers in NetworkManager.cpp, from network
    // threads. Both only queue, so the main thread is the one that acts.
    void pushQueryResult(const std::string& name, const std::string& format, const std::string& sizeStr,
                         const std::string& ip, const std::string& realIp, bool isDedicated, bool useP2P,
                         bool matchInProgress = false);
    void setAuthResult(AuthStatus status, const std::string& message, const std::string& username = "");
    void onAuthSuccess(const std::string& username, const std::string& token);

private:
    NetworkManager() = default;
    ~NetworkManager() = default;

    // Join attempts run on detached threads, so the backend is swapped from a
    // thread other than the one driving it. joinGeneration invalidates an
    // attempt the moment a newer one starts, so a slow attempt that finishes
    // late cannot install itself over the current backend.
    void setBackend(std::shared_ptr<GameBackend> next);
    uint64_t beginJoin();
    bool installBackend(std::shared_ptr<GameBackend> next, uint64_t generation);
    // Points a fresh backend's callbacks at ours.
    void wireBackend(const std::shared_ptr<GameBackend>& next);

    // Keeps a background browser client alive until clearQueries().
    void trackQueryClient(std::shared_ptr<znet::Client> client);
    // Login and registration are the same request/reply, so they share a body.
    void runAuthRequest(const std::string& pendingMessage, std::function<std::shared_ptr<znet::Packet>()> makeRequest);

    mutable std::mutex backendMutex;
    uint64_t joinGeneration = 0;
    std::shared_ptr<GameBackend> backend;
    std::shared_ptr<LobbyStatePacket> currentLobbyState;
    bool wantsDisconnect = false;
    // A disconnect that arrived with no handler registered, replayed by
    // setOnDisconnected once one is.
    bool disconnectPending = false;

    struct QueryResult {
        std::string name;
        std::string format;
        std::string sizeStr;
        std::string ip;
        std::string realIp;
        bool isDedicated;
        bool useP2P;
        bool matchInProgress;
    };
    std::mutex queryMutex;
    std::vector<QueryResult> pendingQueries;
    std::vector<std::shared_ptr<znet::Client>> queryClients; // For background server browser pings

    std::atomic<AuthStatus> currentAuthStatus{AUTH_NONE};
    mutable std::mutex authMutex;
    std::string authMessageText;
    std::string authUsername;
    std::string sessionEmail;
    std::string sessionToken;

    bool hostMode = false;
    uint8_t lobbyTeamSize = 2;
    std::string localPlayerName;
    std::atomic<VoiceChatMode> voiceMode{VOICE_MODE_PUSH_TO_TALK};
    std::atomic<bool> hearEnemiesVoice{false};
    std::atomic<bool> proximityChatEnabled{true};
};
