package main

import (
	"database/sql"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"

	_ "github.com/lib/pq"

	"github.com/joho/godotenv"
)

type Puzzle struct {
	ID     string `json:"id"`
	FEN    string `json:"fen"`
	Moves  string `json:"moves"`
	Rating int    `json:"rating"`
}

func getRandomPuzzle(db *sql.DB) (*Puzzle, error) {
	//                                                    Table "public.lichess_puzzles"
	//       Column      |           Type           | Collation | Nullable | Default  | Storage  | Compression | Stats target | Description
	// ------------------+--------------------------+-----------+----------+----------+----------+-------------+--------------+-------------
	//  puzzle_id        | text                     |           | not null |          | extended |             |              |
	//  fen              | text                     |           | not null |          | extended |             |              |
	//  moves            | text                     |           | not null |          | extended |             |              |
	//  rating           | integer                  |           | not null |          | plain    |             |              |
	// Indexes:
	//     "lichess_puzzles_pkey" PRIMARY KEY, btree (puzzle_id)
	//     "lichess_puzzles_rating_idx" btree (rating)

	var puzzle Puzzle
	err := db.QueryRow("SELECT puzzle_id, fen, moves, rating FROM lichess_puzzles ORDER BY RANDOM() LIMIT 1").Scan(
		&puzzle.ID, &puzzle.FEN, &puzzle.Moves, &puzzle.Rating)
	if err != nil {
		return nil, err
	}

	return &puzzle, nil
}

func main() {
	_ = godotenv.Load()

	db_url, ok := os.LookupEnv("DB_URL")
	if !ok {
		log.Fatal("DB_URL not set")
	}
	db, err := sql.Open("postgres", db_url)
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()

	// db.Open() only creates a connection pool, and doesn't actually establish
	// a connection. To ensure the connection works you need to do *something*
	// with a connection.
	err = db.Ping()
	if err != nil {
		log.Fatal(err)
	}

	r := chi.NewRouter()
	r.Use(middleware.Logger)
	r.Use(middleware.Compress(5, "application/json"))
	r.Use(middleware.Timeout(time.Second * 30))

	r.Get("/health", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("OK"))
	})

	r.Get("/puzzle", func(w http.ResponseWriter, r *http.Request) {
		puzzle, err := getRandomPuzzle(db)

		if err != nil {
			log.Println("Error getting random puzzle:", err)
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(puzzle)
	})

	http.ListenAndServe(":3000", r)
}
