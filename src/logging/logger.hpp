#pragma once

#include <string>

namespace anolis_provider_bread::logging {

enum class Level {
    Info,
    Warning,
    Error,
};

void log(Level level, const std::string &message);
void info(const std::string &message);
void warning(const std::string &message);
void error(const std::string &message);

} // namespace anolis_provider_bread::logging
