#include "logging/logger.hpp"

#include <iostream>

namespace anolis_provider_bread::logging {
namespace {

const char *to_prefix(Level level) {
    switch(level) {
    case Level::Info:
        return "[INFO]";
    case Level::Warning:
        return "[WARN]";
    case Level::Error:
        return "[ERROR]";
    }

    return "[LOG]";
}

std::ostream &stream_for(Level level) {
    return level == Level::Error ? std::cerr : std::clog;
}

} // namespace

void log(Level level, const std::string &message) {
    stream_for(level) << to_prefix(level) << ' ' << message << '\n';
}

void info(const std::string &message) {
    log(Level::Info, message);
}

void warning(const std::string &message) {
    log(Level::Warning, message);
}

void error(const std::string &message) {
    log(Level::Error, message);
}

} // namespace anolis_provider_bread::logging
