package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"log"
	"log/slog"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/go-chi/httplog/v3"

	_ "github.com/lib/pq"

	"github.com/joho/godotenv"
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

	dbURL, ok := os.LookupEnv("DB_URL")
	if !ok {
		logger.Error("DB_URL not set")
		os.Exit(1)
	}

	db, err := sql.Open("postgres", dbURL)
	if err != nil {
		logger.Error("failed to open database", slog.String("error", err.Error()))
		os.Exit(1)
	}
	defer db.Close()

	if err := db.Ping(); err != nil {
		logger.Error("failed to ping database", slog.String("error", err.Error()))
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
	r.Get("/puzzle", handleGetPuzzle(db, logger))

	addr := ":3000"
	logger.Info("server starting", slog.String("addr", addr))

	if err := http.ListenAndServe(addr, r); err != nil {
		logger.Error("server stopped", slog.String("error", err.Error()))
		os.Exit(1)
	}
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

	handler := slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: level,
	})

	return slog.New(handler).With(
		slog.String("service", "chess-api"),
	)
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	w.Write([]byte("OK"))
}

func handleGetPuzzle(db *sql.DB, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		puzzle, err := getRandomPuzzle(db)
		if err != nil {
			httplog.SetError(r.Context(), err)

			logger.ErrorContext(
				r.Context(),
				"failed to get random puzzle",
				slog.String("error", err.Error()),
			)

			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/json")

		if err := json.NewEncoder(w).Encode(puzzle); err != nil {
			httplog.SetError(r.Context(), err)

			logger.ErrorContext(
				r.Context(),
				"failed to encode puzzle response",
				slog.String("error", err.Error()),
			)
		}
	}
}

func getRandomPuzzle(db *sql.DB) (*Puzzle, error) {
	var puzzle Puzzle

	err := db.QueryRow(`
		SELECT puzzle_id, fen, moves, rating
		FROM lichess_puzzles
		ORDER BY RANDOM()
		LIMIT 1
	`).Scan(
		&puzzle.ID,
		&puzzle.FEN,
		&puzzle.Moves,
		&puzzle.Rating,
	)
	if err != nil {
		return nil, fmt.Errorf("query random puzzle: %w", err)
	}

	return &puzzle, nil
}