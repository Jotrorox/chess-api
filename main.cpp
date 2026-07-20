#include "dotenv.hpp"
#include "httplib.h"
#include "logger.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    dotenv::load();

    auto& logger = slog::Logger::instance();

    constexpr slog::Level default_log_level = slog::Level::warning;
    logger.set_level(default_log_level);
    if (const char* log_level_env = std::getenv("LOG_LEVEL")) {
        const auto log_level = slog::level_from_string(log_level_env);
        if (log_level) {
            logger.set_level(*log_level);
        } else {
            SLOG_WARNING(
                "Invalid LOG_LEVEL '\"", log_level_env,
                "\"'; using WARNING"
            );
        }
    }
    logger.enable_colors(true);
    logger.enable_console(true);

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res){
        res.set_content("Hello!", "text/plain");
    });

    const char* port_env = std::getenv("PORT");
    const int port = port_env ? std::stoi(port_env) : 8080;

    SLOG_INFO("The server now started on: ", port);

    svr.listen("0.0.0.0", port);
}
