/*
 * gipMultiplayer.h
 *
 * High-level GlistEngine Multiplayer Plugin Interface.
 * Encapsulates the underlying znet transport behind NetworkManager and
 * NetworkSynchronizer, and provides generic matchmaking, P2P NAT hole
 * punching, and 20 Hz state replication.
 */

#ifndef GIPMULTIPLAYER_H
#define GIPMULTIPLAYER_H

#include "gBasePlugin.h"
#include "gipMultiplayerTypes.h"
#include "NetworkManager.h"
#include "NetworkSynchronizer.h"

class gipMultiplayer : public gBasePlugin {
public:
	gipMultiplayer() = default;
	virtual ~gipMultiplayer() = default;

	static NetworkManager* getNetworkManager() { return NetworkManager::getInstance(); }
	static NetworkSynchronizer* getSynchronizer() { return NetworkSynchronizer::getInstance(); }
};

#endif // GIPMULTIPLAYER_H
