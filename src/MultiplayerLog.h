/*
 * MultiplayerLog.h
 *
 * Console output for the multiplayer plugin, and the gate that quiets it.
 *
 * Two sources print during a match: znet, which logs at Debug level by
 * default, and the plugin itself. Both are routed through here so a single
 * flag controls them, and so failures stay visible when progress chatter is
 * turned off. Set the flag with NetworkManager::setVerboseLogging().
 */

#ifndef GIPMULTIPLAYER_MULTIPLAYERLOG_H
#define GIPMULTIPLAYER_MULTIPLAYERLOG_H

#include <sstream>
#include <string>

namespace gipmp {

/*
 * Progress chatter: connection attempts, players joining, teams changing.
 * Printed only while verbose logging is on.
 */
void mpLogInfo(const std::string& message);

/*
 * Failures: a punch that never landed, a rejected join, a socket that would
 * not open. Always printed, because these are the lines that explain why a
 * match did not start.
 */
void mpLogError(const std::string& message);

/*
 * Off by default. Turning it on also lets znet's Debug and Info records
 * through; znet warnings and errors are never suppressed either way.
 *
 * Safe to call before any networking starts, which is the intended use.
 */
void setVerboseLogging(bool verbose);
bool isVerboseLogging();

}  // namespace gipmp

/*
 * Stream-style wrappers, so a call site keeps the shape it had as a
 * std::cout line:
 *
 *     MP_LOG_INFO("[P2PClient] Connecting to " << ip << ":" << port);
 *
 * MP_LOG_INFO builds nothing at all while quiet, which keeps the cost of a
 * silenced line to one atomic load.
 */
#define MP_LOG_INFO(expr)                     \
    do {                                      \
        if (::gipmp::isVerboseLogging()) {    \
            ::std::ostringstream mplogstream; \
            mplogstream << expr;              \
            ::gipmp::mpLogInfo(mplogstream.str()); \
        }                                     \
    } while (false)

#define MP_LOG_ERROR(expr)                    \
    do {                                      \
        ::std::ostringstream mplogstream;     \
        mplogstream << expr;                  \
        ::gipmp::mpLogError(mplogstream.str()); \
    } while (false)

/*
 * Gate for the engine's own logger, for call sites that should keep the
 * [INFO] Tag: formatting and whatever routing gLogi already has:
 *
 *     MP_GLOGI("GameBackendLocal") << "[Host] Punch socket open on " << port;
 *
 * Written as if/else rather than a bare if so it cannot swallow a trailing
 * else at the call site, and so the streamed arguments are not evaluated
 * while quiet. Warnings keep using gLogw directly: they always print.
 */
#define MP_GLOGI(tag)                     \
    if (!::gipmp::isVerboseLogging()) {   \
    } else                                \
        gLogi(tag)

#endif  // GIPMULTIPLAYER_MULTIPLAYERLOG_H
