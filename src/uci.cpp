/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord
  Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "evaluate.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"

using namespace std;

extern void benchmark(const Position &pos, istream &is);

namespace {
constexpr char kDoubleQuote = '\"';
constexpr char kSpace = ' ';

// FEN string of the initial position, normal chess
const char *StartFEN = "3r3k/p5pp/8/8/5P2/3Qp1P1/P2p3P/3R2K1 b - - 0 33";

// A list to keep track of the position states along the setup moves (from the
// start position to the position just before the search starts). Needed by
// 'draw by repetition' detection.
StateListPtr States(new std::deque<StateInfo>(1));

// position() is called when engine receives the "position" UCI command.
// The function sets up the position described in the given FEN string ("fen")
// or the starting position ("startpos") and then makes the moves given in the
// following move list ("moves").

void position(Position &pos, istringstream &is) {

  Move m;
  string token, fen;

  is >> token;

  if (token == "startpos") {
    fen = StartFEN;
    is >> token; // Consume "moves" token if any
  } else if (token == "fen")
    while (is >> token && token != "moves")
      fen += token + " ";
  else
    return;

  States = StateListPtr(new std::deque<StateInfo>(1));
  pos.set(fen, Options["UCI_Chess960"], &States->back(), Threads.main());

  // Parse move list (if any)
  while (is >> token && (m = UCI::to_move(pos, token)) != MOVE_NONE) {
    States->push_back(StateInfo());
    pos.do_move(m, States->back());
  }
}

// setoption() is called when engine receives the "setoption" UCI command. The
// function updates the UCI option ("name") to the given value ("value").

void setoption(istringstream &is) {

  string token, name, value;

  is >> token; // Consume "name" token

  // Read option name (can contain spaces)
  while (is >> token && token != "value")
    name += string(" ", name.empty() ? 0 : 1) + token;

  // Read option value (can contain spaces)
  while (is >> token)
    value += string(" ", value.empty() ? 0 : 1) + token;

  if (Options.count(name))
    Options[name] = value;
  else
    sync_cout << "No such option: " << name << sync_endl;
}

// go() is called when engine receives the "go" UCI command. The function sets
// the thinking time and other parameters from the input string, then starts
// the search.

void go(const string &fen) {
  cout << kDoubleQuote << fen << kDoubleQuote << kSpace;
  Position pos;
  States = StateListPtr(new std::deque<StateInfo>(1));
  pos.set(fen, false, &States->back(), Threads.main());
  Search::LimitsType limits;
  limits.startTime = now(); // As early as possible!
  Threads.start_thinking(pos, States, limits);
  Threads.main()->wait_for_search_finished();
}

// On ucinewgame following steps are needed to reset the state
void newgame() {
  TT.resize(Options["Hash"]);
  Search::clear();
  Tablebases::init(Options["SyzygyPath"]);
  Time.availableNodes = 0;
}

// Parses a file of FEN strings.
vector<string> ParseGameFile(const string &file) {
  vector<string> tmp;
  ifstream in_file(file);
  string line;
  while (getline(in_file, line)) {
    tmp.push_back(line);
  }
  return tmp;
}

} // namespace

// Generates {FEN,label} pairs from the @kDefaultGame PGN file.
void UCI::loop(int argc, char *argv[]) {
  const string kDefaultGame = "/Users/carlom/Desktop/scratch/game.txt";
  const string game_txt_file(kDefaultGame);
  vector<string> fens = ParseGameFile(game_txt_file);
  newgame(); // Implied ucinewgame before the first position command
  for (const auto &fen : fens) {
    go(fen);
  }
}

/// UCI::value() converts a Value to a string suitable for use with the UCI
/// protocol specification:
///
/// cp <x>    The score from the engine's point of view in centipawns.
/// mate <y>  Mate in y moves, not plies. If the engine is getting mated
///           use negative values for y.

string UCI::value(Value v) {

  assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

  stringstream ss;

  if (abs(v) < VALUE_MATE - MAX_PLY)
    ss << "cp " << v * 100 / PawnValueEg;
  else
    ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

  return ss.str();
}

/// UCI::square() converts a Square to a string in algebraic notation (g1, a7,
/// etc.)

std::string UCI::square(Square s) {
  return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

/// UCI::move() converts a Move to a string in coordinate notation (g1f3,
/// a7a8q). The only special case is castling, where we print in the e1g1
/// notation in normal chess mode, and in e1h1 notation in chess960 mode.
/// Internally all castling moves are always encoded as 'king captures rook'.

string UCI::move(Move m, bool chess960) {

  Square from = from_sq(m);
  Square to = to_sq(m);

  if (m == MOVE_NONE)
    return "(none)";

  if (m == MOVE_NULL)
    return "0000";

  if (type_of(m) == CASTLING && !chess960)
    to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

  string move = UCI::square(from) + UCI::square(to);

  if (type_of(m) == PROMOTION)
    move += " pnbrqk"[promotion_type(m)];

  return move;
}

/// UCI::to_move() converts a string representing a move in coordinate notation
/// (g1f3, a7a8q) to the corresponding legal Move, if any.

Move UCI::to_move(const Position &pos, string &str) {

  if (str.length() == 5) // Junior could send promotion piece in uppercase
    str[4] = char(tolower(str[4]));

  for (const auto &m : MoveList<LEGAL>(pos))
    if (str == UCI::move(m, pos.is_chess960()))
      return m;

  return MOVE_NONE;
}
