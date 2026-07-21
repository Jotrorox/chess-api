#include "dotenv.hpp"
#include "httplib.h"
#include "logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

constexpr char puzzle_database_url[] =
    "https://database.lichess.org/lichess_db_puzzle.csv.zst";

void download_puzzle_database(const std::filesystem::path& download_path) {
    if (std::filesystem::exists(download_path)) {
        if (!std::filesystem::is_regular_file(download_path)) {
            throw std::runtime_error(
                "DOWNLOAD_PATH exists but is not a regular file: " +
                download_path.string()
            );
        }

        SLOG_INFO("Puzzle database already present at: ", download_path.string());
        return;
    }

    const auto parent_path = download_path.parent_path();
    if (!parent_path.empty()) {
        std::filesystem::create_directories(parent_path);
    }

    const auto temporary_path = download_path.string() + ".part";
    std::error_code error;
    std::filesystem::remove(temporary_path, error);

    SLOG_INFO("Downloading puzzle database to: ", download_path.string());
    const auto pid = fork();
    if (pid == -1) {
        throw std::runtime_error("Unable to start curl for puzzle database download");
    }

    if (pid == 0) {
        execlp(
            "curl", "curl", "--fail", "--location", "--output",
            temporary_path.c_str(), puzzle_database_url,
            static_cast<char*>(nullptr)
        );
        _exit(EXIT_FAILURE);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != EXIT_SUCCESS) {
        std::filesystem::remove(temporary_path, error);
        throw std::runtime_error("Puzzle database download failed");
    }

    std::filesystem::rename(temporary_path, download_path);
    SLOG_INFO("Puzzle database downloaded to: ", download_path.string());
}

void unpack_puzzle_database(const std::filesystem::path& archive_path) {
    auto data_path = archive_path;
    if (data_path.extension() != ".zst") {
        throw std::runtime_error("DOWNLOAD_PATH must end in .zst");
    }
    data_path.replace_extension();

    if (std::filesystem::exists(data_path)) {
        if (!std::filesystem::is_regular_file(data_path)) {
            throw std::runtime_error(
                "Unpacked data path exists but is not a regular file: " +
                data_path.string()
            );
        }

        SLOG_INFO("Unpacked puzzle data already present at: ", data_path.string());
        return;
    }

    const auto temporary_path = data_path.string() + ".part";
    std::error_code error;
    std::filesystem::remove(temporary_path, error);

    SLOG_INFO("Unpacking puzzle database to: ", data_path.string());
    const auto pid = fork();
    if (pid == -1) {
        throw std::runtime_error("Unable to start zstd");
    }

    if (pid == 0) {
        execlp(
            "zstd", "zstd", "--decompress", archive_path.c_str(),
            "-o", temporary_path.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(EXIT_FAILURE);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != EXIT_SUCCESS) {
        std::filesystem::remove(temporary_path, error);
        throw std::runtime_error("Puzzle database unpacking failed");
    }

    std::filesystem::rename(temporary_path, data_path);
    SLOG_INFO("Puzzle database unpacked to: ", data_path.string());
}

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

    const char* download_path_env = std::getenv("DOWNLOAD_PATH");
    if (!download_path_env || *download_path_env == '\0') {
        SLOG_ERROR("DOWNLOAD_PATH must be set");
        return EXIT_FAILURE;
    }

    try {
        download_puzzle_database(download_path_env);
        unpack_puzzle_database(download_path_env);
    } catch (const std::exception& error) {
        SLOG_ERROR(error.what());
        return EXIT_FAILURE;
    }

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res){
        res.set_content("Hello!", "text/plain");
    });

    const char* port_env = std::getenv("PORT");
    const int port = port_env ? std::stoi(port_env) : 8080;

    SLOG_INFO("The server now started on: ", port);

    svr.listen("0.0.0.0", port);
}
