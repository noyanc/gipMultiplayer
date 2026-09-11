#include "MultiplayerLog.h"

#include "znet/logger.h"

#include <atomic>
#include <iostream>
#include <mutex>

namespace gipmp {
namespace {

std::atomic<bool> g_verbose{false};

/*
 * Records can arrive from any znet thread, so the two halves of a line must
 * not interleave with another thread's.
 */
std::mutex& outputMutex() {
    static std::mutex mutex;
    return mutex;
}

/*
 * znet colours its own output by baking escapes into the format string, which
 * a sink never sees: it is handed the message as plain text so that a
 * structured logger downstream is not left parsing escapes back out. Printing
 * to a console, the colours are worth keeping, so the same scheme is rebuilt
 * here: the level on a coloured background, the function in magenta, and the
 * message itself red for warnings and errors.
 */
struct LevelStyle {
    const char* label;
    const char* labelColor;
    const char* messageColor;
};

LevelStyle styleFor(znet::LogLevel level) {
    switch (level) {
        case znet::LogLevel::Debug: return {"[debug]", "\x1b[44m", "\x1b[0m"};
        case znet::LogLevel::Info:  return {"[info ]", "\x1b[42m", "\x1b[0m"};
        case znet::LogLevel::Warn:  return {"[warn ]", "\x1b[41m", "\x1b[31m"};
        case znet::LogLevel::Error: return {"[error]", "\x1b[41m", "\x1b[31m"};
    }
    return {"[?????]", "\x1b[45m", "\x1b[0m"};
}

/*
 * Debug and Info are chatter and answer to the flag. Warn and Error always
 * print: a dropped warning is a networking fault nobody sees.
 */
void writeZnetRecord(znet::LogLevel level, const char* function,
                     const char* message, void* user) {
    (void) user;

    bool chatter = level == znet::LogLevel::Debug || level == znet::LogLevel::Info;
    if (chatter && !g_verbose.load(std::memory_order_relaxed)) {
        return;
    }

    const LevelStyle style = styleFor(level);
    std::lock_guard<std::mutex> lock(outputMutex());
    std::cout << style.labelColor << style.label << "\x1b[0m "
              << "\x1b[35m" << (function != nullptr ? function : "") << ": "
              << style.messageColor << (message != nullptr ? message : "")
              << "\x1b[0m" << std::endl;
}

/*
 * Static storage, because znet keeps the pointer and reads it on every log
 * call for the rest of the process.
 */
const znet::LogSink& znetSink() {
    static znet::LogSink sink{&writeZnetRecord, nullptr};
    return sink;
}

/*
 * znet logs from its own constructors, so the sink has to be in place before
 * the first network object exists rather than at the first setVerboseLogging()
 * call. Installing it from a namespace-scope initializer covers that; the sink
 * only ever consults g_verbose, so it costs nothing while quiet.
 */
struct SinkInstaller {
    SinkInstaller() { znet::SetLogSink(&znetSink()); }
};

SinkInstaller g_sinkInstaller;

}  // namespace

void mpLogInfo(const std::string& message) {
    if (!g_verbose.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(outputMutex());
    std::cout << message << std::endl;
}

void mpLogError(const std::string& message) {
    std::lock_guard<std::mutex> lock(outputMutex());
    std::cout << message << std::endl;
}

void setVerboseLogging(bool verbose) {
    g_verbose.store(verbose, std::memory_order_relaxed);
}

bool isVerboseLogging() {
    return g_verbose.load(std::memory_order_relaxed);
}

}  // namespace gipmp
