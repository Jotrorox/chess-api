#include "dotenv.hpp"
#include "httplib.h"
#include "logger.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

constexpr char puzzle_database_url[] =
    "https://database.lichess.org/lichess_db_puzzle.csv.zst";

class SqliteDatabase {
public:
    explicit SqliteDatabase(const std::filesystem::path& path) {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        if (sqlite3_open_v2(path.c_str(), &handle_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = handle_ ? sqlite3_errmsg(handle_) : "unknown error";
            if (handle_) sqlite3_close(handle_);
            handle_ = nullptr;
            throw std::runtime_error("Unable to open SQLite database: " + message);
        }
        sqlite3_busy_timeout(handle_, 30000);
    }

    ~SqliteDatabase() { if (handle_) sqlite3_close(handle_); }
    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    sqlite3* get() const { return handle_; }

    void execute(const char* sql) const {
        char* error = nullptr;
        if (sqlite3_exec(handle_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(handle_);
            sqlite3_free(error);
            throw std::runtime_error("SQLite error: " + message);
        }
    }

private:
    sqlite3* handle_ = nullptr;
};

class SqliteStatement {
public:
    SqliteStatement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &handle_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~SqliteStatement() { sqlite3_finalize(handle_); }
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    sqlite3_stmt* get() const { return handle_; }

private:
    sqlite3_stmt* handle_ = nullptr;
};

std::string json_string(const unsigned char* value) {
    const std::string_view input = value
        ? reinterpret_cast<const char*>(value)
        : "";
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : input) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    output << "\\u00" << hex[character >> 4] << hex[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

class PuzzleRepository {
public:
    explicit PuzzleRepository(const std::filesystem::path& path) : database_(path) {
        database_.execute("PRAGMA cache_size=-1024;PRAGMA mmap_size=0;");
        SqliteStatement statement(
            database_.get(),
            "SELECT value FROM app_metadata WHERE key = 'puzzle_count'"
        );
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            throw std::runtime_error("Puzzle count metadata is missing");
        }
        count_ = sqlite3_column_int64(statement.get(), 0);
        if (count_ <= 0) throw std::runtime_error("Puzzle database is empty");
    }

    std::string random_puzzle() const {
        thread_local std::mt19937_64 generator(std::random_device{}());
        return puzzle_at_or_after(random_key(generator));
    }

    std::string daily_puzzle() const {
        using namespace std::chrono;
        const auto utc_day = duration_cast<hours>(
            system_clock::now().time_since_epoch()).count() / 24;
        std::mt19937_64 generator(stable_hash(static_cast<std::uint64_t>(utc_day)));
        return puzzle_at_or_after(random_key(generator));
    }

    std::int64_t count() const { return count_; }

    bool is_healthy() const {
        SqliteStatement statement(database_.get(), "SELECT 1 FROM puzzles LIMIT 1");
        return sqlite3_step(statement.get()) == SQLITE_ROW;
    }

private:
    static std::string random_key(std::mt19937_64& generator) {
        // Puzzle IDs use ASCII-ordered base62 characters. Seeking to a random
        // key keeps selection indexed instead of scanning millions of rows.
        constexpr char alphabet[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::uniform_int_distribution<std::size_t> distribution(0, 61);
        std::string key(5, '0');
        for (char& character : key) character = alphabet[distribution(generator)];
        return key;
    }

    static std::uint64_t stable_hash(std::uint64_t value) {
        // SplitMix64 gives a stable, well-distributed mapping from UTC day to offset.
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    std::string puzzle_at_or_after(const std::string& puzzle_id) const {
        constexpr char sql[] =
            "SELECT puzzle_id, fen, moves, rating, rating_deviation, popularity, "
            "play_count, themes, game_url, opening_tags "
            "FROM puzzles WHERE puzzle_id >= ? ORDER BY puzzle_id LIMIT 1";
        SqliteStatement statement(database_.get(), sql);
        if (sqlite3_bind_text(statement.get(), 1, puzzle_id.data(),
                              static_cast<int>(puzzle_id.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_.get()));
        }

        int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return first_puzzle();
        }
        if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_.get()));

        return puzzle_json(statement.get());
    }

    std::string first_puzzle() const {
        constexpr char sql[] =
            "SELECT puzzle_id, fen, moves, rating, rating_deviation, popularity, "
            "play_count, themes, game_url, opening_tags "
            "FROM puzzles ORDER BY puzzle_id LIMIT 1";
        SqliteStatement statement(database_.get(), sql);
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database_.get()));
        }
        return puzzle_json(statement.get());
    }

    static std::string puzzle_json(sqlite3_stmt* statement) {
        std::ostringstream json;
        json << "{\"id\":" << json_string(sqlite3_column_text(statement, 0))
             << ",\"fen\":" << json_string(sqlite3_column_text(statement, 1))
             << ",\"moves\":" << json_string(sqlite3_column_text(statement, 2))
             << ",\"rating\":" << sqlite3_column_int(statement, 3)
             << ",\"ratingDeviation\":" << sqlite3_column_int(statement, 4)
             << ",\"popularity\":" << sqlite3_column_int(statement, 5)
             << ",\"playCount\":" << sqlite3_column_int64(statement, 6)
             << ",\"themes\":" << json_string(sqlite3_column_text(statement, 7))
             << ",\"gameUrl\":" << json_string(sqlite3_column_text(statement, 8))
             << ",\"openingTags\":" << json_string(sqlite3_column_text(statement, 9))
             << '}';
        return json.str();
    }

    SqliteDatabase database_;
    std::int64_t count_ = 0;
};

// Reads exactly one RFC 4180-style record without retaining any previous records.
bool read_csv_record(std::istream& input, std::array<std::string, 10>& fields) {
    for (auto& field : fields) field.clear();
    std::size_t column = 0;
    bool quoted = false;
    bool saw_data = false;

    for (char character; input.get(character);) {
        saw_data = true;
        if (quoted) {
            if (character == '"') {
                if (input.peek() == '"') {
                    input.get(character);
                    fields[column].push_back('"');
                } else {
                    quoted = false;
                }
            } else {
                fields[column].push_back(character);
            }
        } else if (character == '"' && fields[column].empty()) {
            quoted = true;
        } else if (character == ',') {
            if (++column >= fields.size()) {
                throw std::runtime_error("CSV record has more than 10 columns");
            }
        } else if (character == '\n') {
            if (column + 1 != fields.size()) {
                throw std::runtime_error("CSV record does not have 10 columns");
            }
            return true;
        } else if (character != '\r') {
            fields[column].push_back(character);
        }
    }

    if (quoted) throw std::runtime_error("CSV ends inside a quoted field");
    if (!saw_data) return false;
    if (column + 1 != fields.size()) {
        throw std::runtime_error("CSV record does not have 10 columns");
    }
    return true;
}

bool is_header(const std::array<std::string, 10>& fields) {
    return fields[0] == "PuzzleId" && fields[1] == "FEN" && fields[2] == "Moves";
}

void discard_file_cache(const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor == -1) return;
    posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
    close(descriptor);
}

void populate_database_if_empty(const std::filesystem::path& csv_path,
                                const std::filesystem::path& database_path) {
    if (!std::filesystem::is_regular_file(csv_path)) {
        SLOG_INFO("CSV file is not present; skipping database import: ", csv_path.string());
        return;
    }

    SqliteDatabase database(database_path);
    database.execute(
        "PRAGMA cache_size=-4096;"
        "PRAGMA mmap_size=0;"
        "PRAGMA temp_store=FILE;"
        "PRAGMA threads=1;"
        "PRAGMA journal_mode=DELETE;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS puzzles ("
        "puzzle_id TEXT PRIMARY KEY NOT NULL, fen TEXT NOT NULL, moves TEXT NOT NULL,"
        "rating INTEGER NOT NULL, rating_deviation INTEGER NOT NULL,"
        "popularity INTEGER NOT NULL, play_count INTEGER NOT NULL,"
        "themes TEXT NOT NULL, game_url TEXT NOT NULL, opening_tags TEXT NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS app_metadata ("
        "key TEXT PRIMARY KEY NOT NULL, value INTEGER NOT NULL"
        ") WITHOUT ROWID;"
    );

    sqlite3_stmt* count_statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), "SELECT 1 FROM puzzles LIMIT 1", -1,
                           &count_statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    const bool populated = sqlite3_step(count_statement) == SQLITE_ROW;
    sqlite3_finalize(count_statement);
    if (populated) {
        SqliteStatement metadata_statement(
            database.get(),
            "SELECT 1 FROM app_metadata WHERE key = 'puzzle_count'"
        );
        if (sqlite3_step(metadata_statement.get()) != SQLITE_ROW) {
            database.execute(
                "INSERT INTO app_metadata(key, value) "
                "SELECT 'puzzle_count', COUNT(*) FROM puzzles;"
                "PRAGMA shrink_memory;"
            );
            discard_file_cache(database_path);
        }
        SLOG_INFO("SQLite database is already populated; skipping CSV import");
        return;
    }

    std::ifstream csv(csv_path, std::ios::binary);
    if (!csv) throw std::runtime_error("Unable to open CSV file: " + csv_path.string());

    constexpr char insert_sql[] =
        "INSERT INTO puzzles VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), insert_sql, -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }

    std::size_t imported = 0;
    try {
        database.execute("BEGIN IMMEDIATE");
        std::array<std::string, 10> fields;
        while (read_csv_record(csv, fields)) {
            if (imported == 0 && is_header(fields)) continue;
            for (int index = 0; index < static_cast<int>(fields.size()); ++index) {
                const int result = (index >= 3 && index <= 6)
                    ? sqlite3_bind_int64(statement, index + 1, std::stoll(fields[index]))
                    : sqlite3_bind_text(statement, index + 1, fields[index].data(),
                                        static_cast<int>(fields[index].size()), SQLITE_TRANSIENT);
                if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            if (sqlite3_step(statement) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            ++imported;
            if (imported % 1000000 == 0) SLOG_INFO("Imported puzzles: ", imported);
        }
        if (imported == 0) throw std::runtime_error("CSV contains no puzzle records");
        SqliteStatement metadata_statement(
            database.get(),
            "INSERT OR REPLACE INTO app_metadata(key, value) VALUES ('puzzle_count', ?)"
        );
        if (sqlite3_bind_int64(metadata_statement.get(), 1,
                               static_cast<sqlite3_int64>(imported)) != SQLITE_OK ||
            sqlite3_step(metadata_statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database.get()));
        }
        database.execute("COMMIT");
    } catch (...) {
        sqlite3_finalize(statement);
        try { database.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
    sqlite3_finalize(statement);
    SLOG_INFO("Finished importing all ", imported, " puzzles into ", database_path.string());
}

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

    std::unique_ptr<PuzzleRepository> puzzles;
    try {
        download_puzzle_database(download_path_env);
        unpack_puzzle_database(download_path_env);
        auto csv_path = std::filesystem::path(download_path_env);
        csv_path.replace_extension();

        const char* database_path_env = std::getenv("SQLITE_DB_PATH");
        if (!database_path_env || *database_path_env == '\0') {
            throw std::runtime_error("SQLITE_DB_PATH must be set");
        }
        populate_database_if_empty(csv_path, database_path_env);
        puzzles = std::make_unique<PuzzleRepository>(database_path_env);
    } catch (const std::exception& error) {
        SLOG_ERROR(error.what());
        return EXIT_FAILURE;
    }

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res){
        res.set_content("Hello!", "text/plain");
    });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("ok", "text/plain");
    });

    svr.Get("/status", [&puzzles](const httplib::Request&, httplib::Response& response) {
        try {
            const bool healthy = puzzles->is_healthy();
            response.status = healthy ? 200 : 503;
            response.set_content(
                "{\"status\":\"" + std::string(healthy ? "ok" : "error") +
                    "\",\"puzzlesLoaded\":" + std::to_string(puzzles->count()) + "}",
                "application/json"
            );
        } catch (const std::exception& error) {
            SLOG_ERROR("Unable to determine service status: ", error.what());
            response.status = 503;
            response.set_content(
                "{\"status\":\"error\",\"puzzlesLoaded\":" +
                    std::to_string(puzzles->count()) + "}",
                "application/json"
            );
        }
    });

    const auto serve_puzzle = [&puzzles](bool daily, httplib::Response& response) {
        try {
            response.set_content(
                daily ? puzzles->daily_puzzle() : puzzles->random_puzzle(),
                "application/json"
            );
            response.set_header("Cache-Control", daily ? "public, max-age=300" : "no-store");
        } catch (const std::exception& error) {
            SLOG_ERROR("Unable to retrieve puzzle: ", error.what());
            response.status = 500;
            response.set_content("{\"error\":\"Unable to retrieve puzzle\"}",
                                 "application/json");
        }
    };

    svr.Get("/puzzle", [&serve_puzzle](const httplib::Request&, httplib::Response& response) {
        serve_puzzle(false, response);
    });
    svr.Get("/puzzle/daily", [&serve_puzzle](const httplib::Request&, httplib::Response& response) {
        serve_puzzle(true, response);
    });

    const char* port_env = std::getenv("PORT");
    const int port = port_env ? std::stoi(port_env) : 8080;

    SLOG_INFO("The server now started on: ", port);

    svr.listen("0.0.0.0", port);
}
