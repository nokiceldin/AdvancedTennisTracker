# Tennis Tracker

A beginner-friendly C++ console application that lets you record and track an entire tennis match point by point — including full scoring logic, tiebreaks, statistics, and exports.

This project was created to combine my two main interests: tennis and programming, and to build something realistic that could be used during a real match or practice session.

---

## Features
- Full tennis scoring system (sets, games, points, deuce/advantage)
- Supports multiple match formats:
  - Best-of-3 full sets (6–6 → tiebreak to 7)
  - Best-of-3 with deciding 10-point match tiebreak
  - Short sets to 4 (tiebreak to 7 at 4–4)
- Enforces correct 1–2–2 serving pattern in tiebreaks
- Tracks all player statistics automatically:
  - First/second serve percentage, aces, double faults, break points, net points, winners, and unforced errors
- Always-visible TV-style scoreboard with server indicator (●)
- Undo last point, show live stats, or view point-by-point history
- Exports match summaries as .txt, .json, and .csv files

---

## Compile and Run
```bash
g++ -std=c++17 -O0 -g tennistracker.cpp -o tennistracker
./tennistracker
``
