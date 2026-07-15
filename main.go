package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/go-chi/httplog/v3"
	"github.com/joho/godotenv"
	_ "modernc.org/sqlite"
)

type Puzzle struct {
	ID     string `json:"id"`
	FEN    string `json:"fen"`
	Moves  string `json:"moves"`
	Rating int    `json:"rating"`
}

func main() {
	_ = godotenv.Load()

	logger := newLogger()
	slog.SetDefault(logger)
	log.SetFlags(0)

	sqlitePath := os.Getenv("SQLITE_DB_PATH")
	if sqlitePath == "" {
		sqlitePath = "./data/chess.db"
	}
	if err := ensureSQLiteDatabase(context.Background(), sqlitePath); err != nil {
		logger.Error("failed to initialize sqlite database", slog.String("error", err.Error()))
		os.Exit(1)
	}

	r := chi.NewRouter()
	r.Use(middleware.RequestID)
	r.Use(middleware.Compress(5, "application/json"))
	r.Use(middleware.Timeout(30 * time.Second))
	r.Use(httplog.RequestLogger(logger, &httplog.Options{
		Level:         slog.LevelInfo,
		Schema:        httplog.SchemaECS.Concise(false),
		RecoverPanics: true,
	}))

	r.Get("/health", handleHealth)
	r.Get("/puzzle", handleGetPuzzle(sqlitePath, logger))
	r.Get("/puzzle/daily", handleGetDailyPuzzle(sqlitePath, logger))

	addr := ":3000"
	logger.Info("server starting", slog.String("addr", addr), slog.String("sqlite_path", sqlitePath))
	if err := http.ListenAndServe(addr, r); err != nil {
		logger.Error("server stopped", slog.String("error", err.Error()))
		os.Exit(1)
	}
}

// ensureSQLiteDatabase creates the local database and its schema when needed.
func ensureSQLiteDatabase(ctx context.Context, path string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create sqlite directory: %w", err)
	}

	db, err := openSQLite(path)
	if err != nil {
		return err
	}
	defer db.Close()

	return createSQLiteSchema(ctx, db)
}

func openSQLite(path string) (*sql.DB, error) {
	// Each request opens and closes its own handle; this process does not retain
	// an idle connection to the attached volume.
	db, err := sql.Open("sqlite", "file:"+path+"?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)")
	if err != nil {
		return nil, fmt.Errorf("open sqlite database: %w", err)
	}
	db.SetMaxOpenConns(1)
	return db, nil
}

func createSQLiteSchema(ctx context.Context, db *sql.DB) error {
	_, err := db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS lichess_puzzles (
			puzzle_id TEXT PRIMARY KEY,
			fen TEXT NOT NULL,
			moves TEXT NOT NULL,
			rating INTEGER NOT NULL
		);
	`)
	if err != nil {
		return fmt.Errorf("create sqlite schema: %w", err)
	}
	return nil
}

func newLogger() *slog.Logger {
	level := slog.LevelInfo
	switch strings.ToLower(strings.TrimSpace(os.Getenv("LOG_LEVEL"))) {
	case "debug":
		level = slog.LevelDebug
	case "info", "":
		level = slog.LevelInfo
	case "warn", "warning":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	}
	return slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: level})).With(slog.String("service", "chess-api"))
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("OK"))
}

func handleGetPuzzle(sqlitePath string, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		puzzle, err := getRandomPuzzle(r.Context(), sqlitePath)
		if err != nil {
			handlePuzzleError(w, r, logger, "failed to get random puzzle", err)
			return
		}
		writePuzzle(w, r, logger, puzzle)
	}
}

func getRandomPuzzle(ctx context.Context, sqlitePath string) (*Puzzle, error) {
	db, err := openSQLite(sqlitePath)
	if err != nil {
		return nil, err
	}
	defer db.Close()
	return queryPuzzle(ctx, db, `SELECT puzzle_id, fen, moves, rating FROM lichess_puzzles ORDER BY RANDOM() LIMIT 1`)
}

func handleGetDailyPuzzle(sqlitePath string, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		puzzle, err := getDailyPuzzle(r.Context(), sqlitePath, SeedFromTodayUTC())
		if err != nil {
			handlePuzzleError(w, r, logger, "failed to get daily puzzle", err)
			return
		}
		writePuzzle(w, r, logger, puzzle)
	}
}

func getDailyPuzzle(ctx context.Context, sqlitePath string, seed int64) (*Puzzle, error) {
	db, err := openSQLite(sqlitePath)
	if err != nil {
		return nil, err
	}
	defer db.Close()

	var count int64
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM lichess_puzzles`).Scan(&count); err != nil {
		return nil, fmt.Errorf("count daily puzzles: %w", err)
	}
	if count == 0 {
		return nil, errors.New("no puzzles available")
	}
	// The offset makes the result stable for a UTC day without relying on a
	// connection-local random seed, which SQLite does not expose.
	offset := seed % count
	return queryPuzzle(ctx, db, `SELECT puzzle_id, fen, moves, rating FROM lichess_puzzles ORDER BY puzzle_id LIMIT 1 OFFSET ?`, offset)
}

func queryPuzzle(ctx context.Context, db *sql.DB, query string, args ...any) (*Puzzle, error) {
	var puzzle Puzzle
	if err := db.QueryRowContext(ctx, query, args...).Scan(&puzzle.ID, &puzzle.FEN, &puzzle.Moves, &puzzle.Rating); err != nil {
		return nil, fmt.Errorf("query puzzle: %w", err)
	}
	return &puzzle, nil
}

func writePuzzle(w http.ResponseWriter, r *http.Request, logger *slog.Logger, puzzle *Puzzle) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(puzzle); err != nil {
		httplog.SetError(r.Context(), err)
		logger.ErrorContext(r.Context(), "failed to encode puzzle response", slog.String("error", err.Error()))
	}
}

func handlePuzzleError(w http.ResponseWriter, r *http.Request, logger *slog.Logger, message string, err error) {
	httplog.SetError(r.Context(), err)
	logger.ErrorContext(r.Context(), message, slog.String("error", err.Error()))
	http.Error(w, "Internal Server Error", http.StatusInternalServerError)
}

func SeedFromTodayUTC() int64 {
	now := time.Now().UTC()
	date := time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, time.UTC)
	return date.Unix() / 86400
}
