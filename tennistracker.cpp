// tennis_tracker.cpp
// Beginner-style interactive tennis match tracker (enforces tiebreak serve 1-2-2)
// Build: g++ -std=c++17 -O0 -g tennis_tracker.cpp -o tennis_tracker

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdlib>

using namespace std;

// =============== Small helpers ===============

static string now_date_time_string() {
    time_t t = time(nullptr);
    tm *lt = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", lt);
    return string(buf);
}

static string safe_percent(int num, int den) {
    if (den <= 0) return "--";
    double p = 100.0 * (double)num / (double)den;
    stringstream ss;
    ss << fixed << setprecision(1) << p << "%";
    return ss.str();
}

static string safe_ratio(int num, int den) {
    stringstream ss;
    ss << num << "/" << den;
    return ss.str();
}

static string left_pad(const string& s, int width) {
    if ((int)s.size() >= width) return s;
    return string(width - (int)s.size(), ' ') + s;
}

static string right_pad(const string& s, int width) {
    if ((int)s.size() >= width) return s;
    return s + string(width - (int)s.size(), ' ');
}

// Optional color (safe fallback)
static bool use_color() {
    const char* term = getenv("TERM");
    if (!term) return false;
    string t(term);
    return (t.find("xterm") != string::npos || t.find("color") != string::npos);
}
static string green_dot() {
    if (use_color()) return "\033[1;32m●\033[0m";
    return "●";
}

// =============== Data ===============

enum ServeType { SERVE_NONE=0, SERVE_FIRST=1, SERVE_SECOND=2 };
enum DecidingSetType { DECIDING_REGULAR=0, DECIDING_TB10=1 };

struct FormatConfig {
    int games_to_win_set;
    int tiebreak_at_games;
    int set_tiebreak_points;   // 7, win by 2
    DecidingSetType deciding;
    int deciding_tb_points;    // 10, win by 2
};

struct PlayerStats {
    // Serve attempts
    int first_serves_attempted=0, first_serves_in=0;
    int second_serves_attempted=0, second_serves_in=0;
    // Serve results
    int aces_first=0, aces_second=0;
    int service_winners_first=0, service_winners_second=0;
    int double_faults=0;
    int points_won_on_first_serve=0, points_won_on_second_serve=0;
    // Return
    int return_points_won_vs_first=0, return_points_won_vs_second=0;
    int return_winners=0, return_unforced_errors=0, return_forced_errors=0;
    // Rally
    int rally_winners=0, unforced_errors=0, forced_errors_drawn=0;
    // Net
    int net_points_won=0, net_points_total=0;
    // Pressure
    int break_points_won=0, break_points_total=0;
    // Totals
    int points_won=0, points_played=0;
};

struct PointLogEntry {
    int set_index=0;
    int game_index=0;
    bool in_tiebreak=false;
    int tiebreak_point_number=0;   // 1-based (only in TB)
    int point_number_in_game=0;    // 1-based (only in regular games)

    int server_player=0;           // who served THIS point
    ServeType serve_type=SERVE_NONE;

    string event_chain;
    int point_winner=0;

    bool was_break_point=false;
    bool was_game_point=false;
    bool was_set_point=false;
    bool was_match_point=false;
};

struct SetScore {
    int games_player1=0;
    int games_player2=0;
    bool set_finished=false;
    bool set_tiebreak_played=false;
    int tb_points_p1=0;
    int tb_points_p2=0;
};

struct MatchState {
    // Meta
    string player1_name, player2_name, location;

    // Format
    FormatConfig format;
    int best_of_sets=3;
    int sets_to_win=2;

    // Sets and per-set stats
    vector<SetScore> sets;
    vector<PlayerStats> per_set_stats_p1;
    vector<PlayerStats> per_set_stats_p2;

    int current_set_index=0;

    // Regular game points (0.. = 0/15/30/40/deuce+)
    int game_points_p1=0, game_points_p2=0;

    // Tiebreak flags and counters
    bool in_set_tiebreak=false;
    bool in_match_tiebreak10=false;
    int tb_points_p1=0, tb_points_p2=0;
    int tb_start_server=0;   // who started the current TB (set TB or TB10)
    // current_server is derived each TB point from tb_start_server + total tb points

    // Serving outside TB
    int current_server=0;    // server for current REGULAR game

    // Sets won
    int sets_won_p1=0, sets_won_p2=0;

    // Match totals
    PlayerStats match_stats_p1, match_stats_p2;

    // in struct MatchState
    ServeType current_point_serve = SERVE_NONE;


    // Log
    vector<PointLogEntry> log_entries;
};

// =============== Globals for undo ===============
static vector<MatchState> history_stack;
static void push_history(const MatchState& st){ history_stack.push_back(st); }
static bool pop_history(MatchState& st){ if(history_stack.empty()) return false; st=history_stack.back(); history_stack.pop_back(); return true; }

// =============== Printing ===============

static string tennis_point_to_string(int p) {
    if (p <= 0) return "0";
    if (p == 1) return "15";
    if (p == 2) return "30";
    if (p >= 3) return "40";
    return "0";
}

static void compute_tiebreak_server(MatchState& st) {
    // Enforce 1–2–2 pattern:
    // Points indexed from 0: 0:S, 1:O, 2:O, 3:S, 4:S, 5:O, 6:O, 7:S, ...
    // So, server = tb_start_server if (i%4==0 or i%4==3), else opponent.
    int total = st.tb_points_p1 + st.tb_points_p2; // points already played
    int mod4 = total % 4;
    int start = st.tb_start_server;
    int opp = (start==0?1:0);
    if (mod4==0 || mod4==3) st.current_server = start;
    else st.current_server = opp;

    // Change ends every 6 points -> we just print a cue when needed (after last point),
    // the user will see it before entering next point.
    if (total>0 && total%6==0) {
        cout << "\n--- Change ends (tiebreak, after " << total << " points) ---\n";
    }
}

static void print_scoreboard(const MatchState& st) {
    string p1 = st.player1_name;
    string p2 = st.player2_name;

    // Serving dot before the server's name
    string sdot = green_dot();
    string name1 = (st.current_server==0 ? (sdot + " " + p1) : ("  " + p1));
    string name2 = (st.current_server==1 ? (sdot + " " + p2) : ("  " + p2));

    int g1 = st.sets[st.current_set_index].games_player1;
    int g2 = st.sets[st.current_set_index].games_player2;

    string pts1, pts2;
    if (st.in_set_tiebreak || st.in_match_tiebreak10) {
        pts1 = to_string(st.tb_points_p1);
        pts2 = to_string(st.tb_points_p2);
    } else {
        if (st.game_points_p1 >= 3 && st.game_points_p2 >= 3) {
            if (st.game_points_p1 == st.game_points_p2) { pts1="40"; pts2="40"; }
            else if (st.game_points_p1 == st.game_points_p2 + 1) { pts1="Ad"; pts2=""; }
            else if (st.game_points_p2 == st.game_points_p1 + 1) { pts1=""; pts2="Ad"; }
            else { pts1=tennis_point_to_string(st.game_points_p1); pts2=tennis_point_to_string(st.game_points_p2); }
        } else {
            pts1 = tennis_point_to_string(st.game_points_p1);
            pts2 = tennis_point_to_string(st.game_points_p2);
        }
    }

    cout << "+--------------------------------------------------+\n";
    cout << "| Location: " << right_pad(st.location, 40) << "|\n";
    cout << "| " << right_pad(name1, 22) << "| " << right_pad(name2, 15) << "|\n";
    cout << "| Sets:           " << left_pad(to_string(st.sets_won_p1), 10)
         << "  | " << left_pad(to_string(st.sets_won_p2), 10) << "|\n";
    cout << "| Games:          " << left_pad(to_string(g1), 10)
         << "  | " << left_pad(to_string(g2), 10) << "|\n";
    cout << "| Points:         " << left_pad(pts1, 10)
         << "  | " << left_pad(pts2, 10) << "|\n";
    cout << "+--------------------------------------------------+\n";
}

// =============== Stats/ratios printing ===============

static void print_single_player_stats(const PlayerStats& s, const string& title) {
    cout << title << "\n";
    cout << "----------------------------------------\n";
    cout << "Serving:\n";
    cout << "  First serve:        " << s.first_serves_in << "/" << s.first_serves_attempted
         << "  (" << safe_percent(s.first_serves_in, s.first_serves_attempted) << ")\n";
    cout << "  1st pts won:        " << s.points_won_on_first_serve << "/" << s.first_serves_in
         << "  (" << safe_percent(s.points_won_on_first_serve, s.first_serves_in) << ")\n";
    cout << "  Second serve:       " << s.second_serves_in << "/" << s.second_serves_attempted
         << "  (" << safe_percent(s.second_serves_in, s.second_serves_attempted) << ")\n";
    cout << "  2nd pts won:        " << s.points_won_on_second_serve << "/" << s.second_serves_in
         << "  (" << safe_percent(s.points_won_on_second_serve, s.second_serves_in) << ")\n";
    cout << "  Aces (1st/2nd):     " << s.aces_first << " / " << s.aces_second << "\n";
    cout << "  Service winners:    " << s.service_winners_first << " / " << s.service_winners_second << "\n";
    cout << "  Double faults:      " << s.double_faults << "\n";

    cout << "Returning:\n";
    cout << "  vs 1st won:         " << s.return_points_won_vs_first << "\n";
    cout << "  vs 2nd won:         " << s.return_points_won_vs_second << "\n";
    cout << "  Return W/UE/FE:     " << s.return_winners << " / " << s.return_unforced_errors << " / " << s.return_forced_errors << "\n";

    cout << "Rallies:\n";
    cout << "  Winners:            " << s.rally_winners << "\n";
    cout << "  Unforced errors:    " << s.unforced_errors << "\n";
    cout << "  Forced drawn:       " << s.forced_errors_drawn << "\n";

    cout << "Net play:\n";
    cout << "  Net points:         " << s.net_points_won << "/" << s.net_points_total
         << "  (" << safe_percent(s.net_points_won, s.net_points_total) << ")\n";

    cout << "Pressure:\n";
    cout << "  Break points:       " << s.break_points_won << "/" << s.break_points_total << "\n";

    cout << "Overall:\n";
    cout << "  Total points:       " << s.points_won << "/" << s.points_played
         << "  (" << safe_percent(s.points_won, s.points_played) << ")\n";
}

static void print_side_by_side(const PlayerStats& a, const PlayerStats& b,
                               const string& nameA, const string& nameB) {
    int L = 32;
    cout << right_pad(nameA, L) << "   " << right_pad(nameB, L) << "\n";
    cout << right_pad(string(32,'-'), L) << "   " << right_pad(string(32,'-'), L) << "\n";
    auto line=[&](const string& la,const string& lb){ cout<<right_pad(la,L)<<"   "<<right_pad(lb,L)<<"\n"; };
    line("First serve:  "+safe_ratio(a.first_serves_in,a.first_serves_attempted)+" ("+safe_percent(a.first_serves_in,a.first_serves_attempted)+")",
         "First serve:  "+safe_ratio(b.first_serves_in,b.first_serves_attempted)+" ("+safe_percent(b.first_serves_in,b.first_serves_attempted)+")");
    line("1st pts won:  "+safe_ratio(a.points_won_on_first_serve,a.first_serves_in)+" ("+safe_percent(a.points_won_on_first_serve,a.first_serves_in)+")",
         "1st pts won:  "+safe_ratio(b.points_won_on_first_serve,b.first_serves_in)+" ("+safe_percent(b.points_won_on_first_serve,b.first_serves_in)+")");
    line("Second srv:   "+safe_ratio(a.second_serves_in,a.second_serves_attempted)+" ("+safe_percent(a.second_serves_in,a.second_serves_attempted)+")",
         "Second srv:   "+safe_ratio(b.second_serves_in,b.second_serves_attempted)+" ("+safe_percent(b.second_serves_in,b.second_serves_attempted)+")");
    line("2nd pts won:  "+safe_ratio(a.points_won_on_second_serve,a.second_serves_in)+" ("+safe_percent(a.points_won_on_second_serve,a.second_serves_in)+")",
         "2nd pts won:  "+safe_ratio(b.points_won_on_second_serve,b.second_serves_in)+" ("+safe_percent(b.points_won_on_second_serve,b.second_serves_in)+")");
    {
        stringstream sa,sb; sa<<"Aces (1/2):   "<<a.aces_first<<" / "<<a.aces_second;
        sb<<"Aces (1/2):   "<<b.aces_first<<" / "<<b.aces_second; line(sa.str(),sb.str());
    }
    {
        stringstream sa,sb; sa<<"Srv winners:  "<<a.service_winners_first<<" / "<<a.service_winners_second;
        sb<<"Srv winners:  "<<b.service_winners_first<<" / "<<b.service_winners_second; line(sa.str(),sb.str());
    }
    {
        stringstream sa,sb; sa<<"Double faults: "<<a.double_faults; sb<<"Double faults: "<<b.double_faults; line(sa.str(),sb.str());
    }
    line("Return vs1st: "+to_string(a.return_points_won_vs_first),
         "Return vs1st: "+to_string(b.return_points_won_vs_first));
    line("Return vs2nd: "+to_string(a.return_points_won_vs_second),
         "Return vs2nd: "+to_string(b.return_points_won_vs_second));
    {
        stringstream sa,sb; sa<<"Return W/UE/FE: "<<a.return_winners<<"/"<<a.return_unforced_errors<<"/"<<a.return_forced_errors;
        sb<<"Return W/UE/FE: "<<b.return_winners<<"/"<<b.return_unforced_errors<<"/"<<b.return_forced_errors; line(sa.str(),sb.str());
    }
    line("Rally winners:"+to_string(a.rally_winners),
         "Rally winners:"+to_string(b.rally_winners));
    line("Unforced err: "+to_string(a.unforced_errors),
         "Unforced err: "+to_string(b.unforced_errors));
    line("Forced drawn: "+to_string(a.forced_errors_drawn),
         "Forced drawn: "+to_string(b.forced_errors_drawn));
    line("Net:          "+safe_ratio(a.net_points_won,a.net_points_total)+" ("+safe_percent(a.net_points_won,a.net_points_total)+")",
         "Net:          "+safe_ratio(b.net_points_won,b.net_points_total)+" ("+safe_percent(b.net_points_won,b.net_points_total)+")");
    line("Break points: "+safe_ratio(a.break_points_won,a.break_points_total),
         "Break points: "+safe_ratio(b.break_points_won,b.break_points_total));
    line("Total points: "+safe_ratio(a.points_won,a.points_played)+" ("+safe_percent(a.points_won,a.points_played)+")",
         "Total points: "+safe_ratio(b.points_won,b.points_played)+" ("+safe_percent(b.points_won,b.points_played)+")");
}

// =============== Scoring helpers ===============

static void start_new_set(class MatchState& st);
static void award_game(MatchState& st, int player);

static bool is_game_point_for(int gp_winner, int gp_loser) {
    if (gp_winner <= 2) return false;
    if (gp_winner == 3 && gp_loser <= 2) return true; // at 40 vs <40
    if (gp_winner >= 3 && gp_loser >= 3) {
        if (gp_winner == gp_loser + 1) return true;   // advantage
    }
    return false;
}

static bool is_break_point_if_receiver_wins(const MatchState& st) {
    int receiver = (st.current_server==0?1:0);
    int gp_receiver = (receiver==0? st.game_points_p1 : st.game_points_p2);
    int gp_server   = (receiver==0? st.game_points_p2 : st.game_points_p1);
    return is_game_point_for(gp_receiver, gp_server);
}

static bool is_set_point_if_player_wins(const MatchState& st, int player) {
    if (st.in_set_tiebreak || st.in_match_tiebreak10) return false;
    const SetScore& ss = st.sets[st.current_set_index];
    int gp_you = (player==0? st.game_points_p1 : st.game_points_p2);
    int gp_opp = (player==0? st.game_points_p2 : st.game_points_p1);
    if (!is_game_point_for(gp_you, gp_opp)) return false;
    int g_you = (player==0? ss.games_player1 : ss.games_player2);
    int g_opp = (player==0? ss.games_player2 : ss.games_player1);
    int gy = g_you + 1;
    if (gy >= st.format.games_to_win_set && (gy - g_opp) >= 2) return true;
    return false;
}

static bool is_match_point_if_player_wins(const MatchState& st, int player) {
    if (st.in_set_tiebreak || st.in_match_tiebreak10) return false;
    if (!is_set_point_if_player_wins(st, player)) return false;
    int sets_have = (player==0? st.sets_won_p1 : st.sets_won_p2);
    return (sets_have == st.sets_to_win - 1);
}

static void add_stats_point_ownership(PlayerStats& winner, PlayerStats& loser) {
    winner.points_won += 1;
    winner.points_played += 1;
    loser.points_played += 1;
}

static void add_stats_net(PlayerStats& who, bool won) {
    who.net_points_total += 1;
    if (won) who.net_points_won += 1;
}

// =============== State transitions ===============

static void start_new_set(MatchState& st) {
    SetScore s;
    st.sets.push_back(s);
    st.per_set_stats_p1.push_back(PlayerStats());
    st.per_set_stats_p2.push_back(PlayerStats());
    st.current_set_index = (int)st.sets.size()-1;

    st.game_points_p1=0; st.game_points_p2=0;
    st.in_set_tiebreak=false;
    st.tb_points_p1=0; st.tb_points_p2=0;
}

static void award_game(MatchState& st, int player) {
    SetScore& ss = st.sets[st.current_set_index];
    if (player==0) ss.games_player1++; else ss.games_player2++;
    // Next game: alternate server
    st.current_server = (st.current_server==0?1:0);
    st.game_points_p1=0; st.game_points_p2=0;
}

static bool check_enter_set_tiebreak(const MatchState& st) {
    const SetScore& ss = st.sets[st.current_set_index];
    return (ss.games_player1==st.format.tiebreak_at_games &&
            ss.games_player2==st.format.tiebreak_at_games);
}

static bool set_is_won_now(const MatchState& st, int &winner) {
    const SetScore& ss = st.sets[st.current_set_index];
    if (st.in_set_tiebreak) {
        if ( (st.tb_points_p1 >= st.format.set_tiebreak_points || st.tb_points_p2 >= st.format.set_tiebreak_points)
             && abs(st.tb_points_p1 - st.tb_points_p2) >= 2 ) {
            winner = (st.tb_points_p1 > st.tb_points_p2 ? 0 : 1);
            return true;
        }
        return false;
    }
    int g1=ss.games_player1, g2=ss.games_player2;
    if ((g1>=st.format.games_to_win_set || g2>=st.format.games_to_win_set) && abs(g1-g2)>=2) {
        winner = (g1>g2?0:1);
        return true;
    }
    return false;
}

static void close_set_and_prepare_next(MatchState& st, int set_winner) {
    st.sets[st.current_set_index].set_finished = true;
    if (set_winner==0) st.sets_won_p1++; else st.sets_won_p2++;

    bool match_over = (st.sets_won_p1==st.sets_to_win || st.sets_won_p2==st.sets_to_win);
    if (!match_over) {
        if ((int)st.sets.size()==2 && st.format.deciding==DECIDING_TB10 &&
            st.sets_won_p1==1 && st.sets_won_p2==1) {
            // Start match TB10
            st.in_match_tiebreak10 = true;
            st.tb_points_p1=0; st.tb_points_p2=0;
            // We will ask who serves first for TB10 in main loop before first point.
        } else {
            start_new_set(st);
        }
    }
}

static bool match_tiebreak10_won(const MatchState& st, int &winner) {
    if (!st.in_match_tiebreak10) return false;
    if ((st.tb_points_p1 >= st.format.deciding_tb_points || st.tb_points_p2 >= st.format.deciding_tb_points)
        && abs(st.tb_points_p1 - st.tb_points_p2) >= 2) {
        winner = (st.tb_points_p1>st.tb_points_p2?0:1);
        return true;
    }
    return false;
}

static bool match_is_over_now(const MatchState& st) {
    return (st.sets_won_p1==st.sets_to_win || st.sets_won_p2==st.sets_to_win);
}

// =============== Stats updates ===============

static void add_serve_attempt(MatchState& st, int server, ServeType t, bool in_or_fault) {
    PlayerStats& ms = (server==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (server==0? st.per_set_stats_p1[st.current_set_index]
                                 : st.per_set_stats_p2[st.current_set_index]);
    if (t==SERVE_FIRST){ ms.first_serves_attempted++; ps.first_serves_attempted++; if(in_or_fault){ms.first_serves_in++; ps.first_serves_in++;} }
    else if (t==SERVE_SECOND){ ms.second_serves_attempted++; ps.second_serves_attempted++; if(in_or_fault){ms.second_serves_in++; ps.second_serves_in++;} }
}
static void add_double_fault(MatchState& st, int server) {
    PlayerStats& ms = (server==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (server==0? st.per_set_stats_p1[st.current_set_index]
                                 : st.per_set_stats_p2[st.current_set_index]);
    ms.double_faults++; ps.double_faults++;
}
static void add_ace(MatchState& st, int server, ServeType t) {
    PlayerStats& ms = (server==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (server==0? st.per_set_stats_p1[st.current_set_index]
                                 : st.per_set_stats_p2[st.current_set_index]);
    if (t==SERVE_FIRST){ ms.aces_first++; ps.aces_first++; }
    else if (t==SERVE_SECOND){ ms.aces_second++; ps.aces_second++; }
}
static void add_service_winner(MatchState& st, int server, ServeType t) {
    PlayerStats& ms = (server==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (server==0? st.per_set_stats_p1[st.current_set_index]
                                 : st.per_set_stats_p2[st.current_set_index]);
    if (t==SERVE_FIRST){ ms.service_winners_first++; ps.service_winners_first++; }
    else if (t==SERVE_SECOND){ ms.service_winners_second++; ps.service_winners_second++; }
}
static void add_return_outcome(MatchState& st, int returner, const string& kind) {
    PlayerStats& ms = (returner==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (returner==0? st.per_set_stats_p1[st.current_set_index]
                                   : st.per_set_stats_p2[st.current_set_index]);
    if (kind=="winner") { ms.return_winners++; ps.return_winners++; }
    else if (kind=="ue") { ms.return_unforced_errors++; ps.return_unforced_errors++; }
    else if (kind=="fe") { ms.return_forced_errors++; ps.return_forced_errors++; }
}
static void add_rally_outcome(MatchState& st, int player, const string& kind) {
    PlayerStats& ms = (player==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (player==0? st.per_set_stats_p1[st.current_set_index]
                                 : st.per_set_stats_p2[st.current_set_index]);
    if (kind=="winner") { ms.rally_winners++; ps.rally_winners++; }
    else if (kind=="ue") { ms.unforced_errors++; ps.unforced_errors++; }
    else if (kind=="fedrawn") { ms.forced_errors_drawn++; ps.forced_errors_drawn++; }
}
static void add_return_points_won(MatchState& st, int returner, ServeType t) {
    PlayerStats& ms = (returner==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (returner==0? st.per_set_stats_p1[st.current_set_index]
                                   : st.per_set_stats_p2[st.current_set_index]);
    if (t==SERVE_FIRST) { ms.return_points_won_vs_first++; ps.return_points_won_vs_first++; }
    else if (t==SERVE_SECOND) { ms.return_points_won_vs_second++; ps.return_points_won_vs_second++; }
}
static void add_server_point_won(MatchState& st, int server, ServeType t) {
    PlayerStats& ms = (server==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (server==0? st.per_set_stats_p1[st.current_set_index]
                                 : st.per_set_stats_p2[st.current_set_index]);
    if (t==SERVE_FIRST) { ms.points_won_on_first_serve++; ps.points_won_on_first_serve++; }
    else if (t==SERVE_SECOND) { ms.points_won_on_second_serve++; ps.points_won_on_second_serve++; }
}
static void maybe_count_break_point(MatchState& st, bool was_bp, bool returner_won) {
    if (!was_bp) return;
    int returner_player = (st.current_server==0?1:0);
    PlayerStats& ms = (returner_player==0? st.match_stats_p1 : st.match_stats_p2);
    PlayerStats& ps = (returner_player==0? st.per_set_stats_p1[st.current_set_index]
                                          : st.per_set_stats_p2[st.current_set_index]);
    ms.break_points_total++; ps.break_points_total++;
    if (returner_won) { ms.break_points_won++; ps.break_points_won++; }
}

// =============== CSV Exports ===============

static void save_csvs(const MatchState& st, const string& base) {
    // 1) Match totals CSV
    {
        ofstream f((base+"_match_totals.csv").c_str());
        if (f) {
            f << "Player,FirstServIn,FirstServAtt,FirstPtsWon,SecondServIn,SecondServAtt,SecondPtsWon,Aces1,Aces2,SrvW1,SrvW2,DF,RetWonV1,RetWonV2,RetW,RetUE,RetFE,RallyW,UE,FEdrawn,NetWon,NetTot,BPWon,BPTot,PtsWon,PtsPlayed\n";
            auto dump=[&](const string& name,const PlayerStats& s){
                f<<name<<","<<s.first_serves_in<<","<<s.first_serves_attempted<<","<<s.points_won_on_first_serve<<","
                 <<s.second_serves_in<<","<<s.second_serves_attempted<<","<<s.points_won_on_second_serve<<","
                 <<s.aces_first<<","<<s.aces_second<<","<<s.service_winners_first<<","<<s.service_winners_second<<","
                 <<s.double_faults<<","<<s.return_points_won_vs_first<<","<<s.return_points_won_vs_second<<","
                 <<s.return_winners<<","<<s.return_unforced_errors<<","<<s.return_forced_errors<<","
                 <<s.rally_winners<<","<<s.unforced_errors<<","<<s.forced_errors_drawn<<","
                 <<s.net_points_won<<","<<s.net_points_total<<","<<s.break_points_won<<","<<s.break_points_total<<","
                 <<s.points_won<<","<<s.points_played<<"\n";
            };
            dump(st.player1_name, st.match_stats_p1);
            dump(st.player2_name, st.match_stats_p2);
        }
    }
    // 2) Per-set CSV
    {
        ofstream f((base+"_per_set_stats.csv").c_str());
        if (f) {
            f << "Set,Player,FirstServIn,FirstServAtt,FirstPtsWon,SecondServIn,SecondServAtt,SecondPtsWon,Aces1,Aces2,SrvW1,SrvW2,DF,RetWonV1,RetWonV2,RetW,RetUE,RetFE,RallyW,UE,FEdrawn,NetWon,NetTot,BPWon,BPTot,PtsWon,PtsPlayed\n";
            for (size_t i=0;i<st.sets.size();i++) {
                auto dump=[&](const string& name,const PlayerStats& s){
                    f<<(i+1)<<","<<name<<","<<s.first_serves_in<<","<<s.first_serves_attempted<<","<<s.points_won_on_first_serve<<","
                     <<s.second_serves_in<<","<<s.second_serves_attempted<<","<<s.points_won_on_second_serve<<","
                     <<s.aces_first<<","<<s.aces_second<<","<<s.service_winners_first<<","<<s.service_winners_second<<","
                     <<s.double_faults<<","<<s.return_points_won_vs_first<<","<<s.return_points_won_vs_second<<","
                     <<s.return_winners<<","<<s.return_unforced_errors<<","<<s.return_forced_errors<<","
                     <<s.rally_winners<<","<<s.unforced_errors<<","<<s.forced_errors_drawn<<","
                     <<s.net_points_won<<","<<s.net_points_total<<","<<s.break_points_won<<","<<s.break_points_total<<","
                     <<s.points_won<<","<<s.points_played<<"\n";
                };
                dump(st.player1_name, st.per_set_stats_p1[i]);
                dump(st.player2_name, st.per_set_stats_p2[i]);
            }
        }
    }
    // 3) Point-by-point CSV
    {
        ofstream f((base+"_points.csv").c_str());
        if (f) {
            f << "Idx,Set,Game,TB,Server,ServeType,Winner,BP,GP,SP,MP,Event\n";
            for (size_t i=0;i<st.log_entries.size();i++) {
                const auto& e=st.log_entries[i];
                f<<(i+1)<<","<<(e.set_index+1)<<","<<(e.game_index+1)<<","<<(e.in_tiebreak?"Y":"N")<<","
                 <<(e.server_player==0?"P1":"P2")<<","
                 <<(e.serve_type==SERVE_FIRST?"1st":(e.serve_type==SERVE_SECOND?"2nd":"-"))<<","
                 <<(e.point_winner==0?"P1":"P2")<<","
                 <<(e.was_break_point?"Y":"N")<<","
                 <<(e.was_game_point?"Y":"N")<<","
                 <<(e.was_set_point?"Y":"N")<<","
                 <<(e.was_match_point?"Y":"N")<<",";
                // naive CSV escaping for commas/quotes
                string ev=e.event_chain;
                for(char& c:ev){ if(c=='"'){ c='\'';
                } }
                f<<"\""<<ev<<"\""<<"\n";
            }
        }
    }
}

// =============== Save TXT/JSON ===============

static void save_match_files(const MatchState& st) {
    string base = st.player1_name + "_vs_" + st.player2_name + "_" + now_date_time_string();
    for (char& c : base) if (c==' ') c='_';

    string txtName = base + ".txt";
    string jsonName = base + ".json";

    ofstream txt(txtName.c_str());
    if (txt) {
        txt << "Match Summary\n=============\n";
        txt << "Players: " << st.player1_name << " vs " << st.player2_name << "\n";
        txt << "Location: " << st.location << "\n";
        txt << "Format: Best-of-3; sets to " << st.format.games_to_win_set
            << " (TB" << st.format.set_tiebreak_points << " at "
            << st.format.tiebreak_at_games << "-" << st.format.tiebreak_at_games << ")";
        if (st.format.deciding==DECIDING_TB10) txt << "; deciding TB10";
        txt << "\n\nFinal Set Scores:\n";
        for (size_t i=0;i<st.sets.size();i++) {
            const SetScore& s = st.sets[i];
            txt << "  Set " << (i+1) << ": " << s.games_player1 << "-" << s.games_player2;
            if (s.set_tiebreak_played) txt << " (TB " << s.tb_points_p1 << "-" << s.tb_points_p2 << ")";
            txt << "\n";
        }
        auto sum_stats=[&](const PlayerStats& s, const string& title){
            txt << "\n" << title << "\n----------------------------------------\n";
            txt << "First serve: " << s.first_serves_in << "/" << s.first_serves_attempted
                << " (" << safe_percent(s.first_serves_in, s.first_serves_attempted) << ")\n";
            txt << "1st pts won: " << s.points_won_on_first_serve << "/" << s.first_serves_in
                << " (" << safe_percent(s.points_won_on_first_serve, s.first_serves_in) << ")\n";
            txt << "Second srv:  " << s.second_serves_in << "/" << s.second_serves_attempted
                << " (" << safe_percent(s.second_serves_in, s.second_serves_attempted) << ")\n";
            txt << "2nd pts won: " << s.points_won_on_second_serve << "/" << s.second_serves_in
                << " (" << safe_percent(s.points_won_on_second_serve, s.second_serves_in) << ")\n";
            txt << "Aces (1/2):  " << s.aces_first << " / " << s.aces_second << "\n";
            txt << "Srv winners: " << s.service_winners_first << " / " << s.service_winners_second << "\n";
            txt << "Double faults: " << s.double_faults << "\n";
            txt << "Return vs1st: " << s.return_points_won_vs_first << "\n";
            txt << "Return vs2nd: " << s.return_points_won_vs_second << "\n";
            txt << "Return W/UE/FE: " << s.return_winners << "/" << s.return_unforced_errors << "/" << s.return_forced_errors << "\n";
            txt << "Rally winners: " << s.rally_winners << "\n";
            txt << "Unforced err: " << s.unforced_errors << "\n";
            txt << "Forced drawn: " << s.forced_errors_drawn << "\n";
            txt << "Net: " << s.net_points_won << "/" << s.net_points_total
                << " (" << safe_percent(s.net_points_won, s.net_points_total) << ")\n";
            txt << "Break points: " << s.break_points_won << "/" << s.break_points_total << "\n";
            txt << "Total points: " << s.points_won << "/" << s.points_played
                << " (" << safe_percent(s.points_won, s.points_played) << ")\n";
        };
        sum_stats(st.match_stats_p1, "Player: "+st.player1_name+" (Match Totals)");
        sum_stats(st.match_stats_p2, "Player: "+st.player2_name+" (Match Totals)");

        txt << "\nPer-set stats\n-------------\n";
        for (size_t i=0;i<st.sets.size();i++) {
            txt << "Set " << (i+1) << ":\n";
            sum_stats(st.per_set_stats_p1[i], "  "+st.player1_name);
            sum_stats(st.per_set_stats_p2[i], "  "+st.player2_name);
        }

        txt << "\nPoint-by-point log\n-------------------\n";
        txt << "# | Set | Game | TB | Server | Serve | Winner | BP/GP/SP/MP | Event\n";
        for (size_t i=0;i<st.log_entries.size();i++) {
            const auto& e=st.log_entries[i];
            txt<<(i+1)<<" | "<<(e.set_index+1)<<" | "<<(e.game_index+1)<<" | "<<(e.in_tiebreak?"Y":"N")
               <<" | "<<(e.server_player==0?st.player1_name:st.player2_name)<<" | "
               <<(e.serve_type==SERVE_FIRST?"1st":(e.serve_type==SERVE_SECOND?"2nd":"-"))<<" | "
               <<(e.point_winner==0?st.player1_name:st.player2_name)<<" | "
               <<(e.was_break_point?"BP":"")<<(e.was_game_point?" GP":"")
               <<(e.was_set_point?" SP":"")<<(e.was_match_point?" MP":"")
               <<" | "<<e.event_chain<<"\n";
        }
        txt.close();
        cout << "Saved text summary: " << txtName << "\n";
    }

    ofstream js(jsonName.c_str());
    if (js) {
        js<<"{\n";
        js<<"  \"players\": [\""<<st.player1_name<<"\", \""<<st.player2_name<<"\"],\n";
        js<<"  \"location\": \""<<st.location<<"\",\n";
        js<<"  \"format\": {\"games_to_win_set\": "<<st.format.games_to_win_set
          <<", \"tiebreak_at_games\": "<<st.format.tiebreak_at_games
          <<", \"set_tiebreak_points\": "<<st.format.set_tiebreak_points
          <<", \"deciding_tb10\": "<<(st.format.deciding==DECIDING_TB10?"true":"false")<<"},\n";
        js<<"  \"sets\": [\n";
        for (size_t i=0;i<st.sets.size();i++) {
            const auto& s=st.sets[i];
            js<<"    {\"p1\": "<<s.games_player1<<", \"p2\": "<<s.games_player2
              <<", \"tb\": "<<(s.set_tiebreak_played?"true":"false")
              <<", \"tb_p1\": "<<s.tb_points_p1<<", \"tb_p2\": "<<s.tb_points_p2<<"}";
            if (i+1<st.sets.size()) js<<",";
            js<<"\n";
        }
        js<<"  ],\n";
        js<<"  \"log\": [\n";
        for (size_t i=0;i<st.log_entries.size();i++) {
            const auto& e=st.log_entries[i];
            js<<"    {\"idx\":"<<(i+1)
              <<", \"set\":"<<(e.set_index+1)
              <<", \"game\":"<<(e.game_index+1)
              <<", \"tb\":"<<(e.in_tiebreak?"true":"false")
              <<", \"server\":"<<(e.server_player==0?"\"P1\"":"\"P2\"")
              <<", \"serve_type\":"<<(e.serve_type==SERVE_FIRST?"\"1st\"":(e.serve_type==SERVE_SECOND?"\"2nd\"":"\"-\""))
              <<", \"winner\":"<<(e.point_winner==0?"\"P1\"":"\"P2\"")
              <<", \"bp\":"<<(e.was_break_point?"true":"false")
              <<", \"gp\":"<<(e.was_game_point?"true":"false")
              <<", \"sp\":"<<(e.was_set_point?"true":"false")
              <<", \"mp\":"<<(e.was_match_point?"true":"false")
              <<", \"event\":\"";
            for(char c: e.event_chain){ if(c=='"') js<<"\\\""; else if(c=='\\') js<<"\\\\"; else js<<c; }
            js<<"\"}";
            if (i+1<st.log_entries.size()) js<<",";
            js<<"\n";
        }
        js<<"  ]\n}\n";
        js.close();
        cout << "Saved JSON data: " << jsonName << "\n";
    }

    // CSV bundle
    save_csvs(st, base);
    cout << "Saved CSVs: " << base << "_match_totals.csv, _per_set_stats.csv, _points.csv\n";
}

// =============== Menus ===============

static void print_format_menu() {
    cout << "Choose match format:\n";
    cout << "  1) Best-of-3 full sets (to 6, TB7 at 6-6)\n";
    cout << "  2) Best-of-3 with match TB10 instead of 3rd set (sets 1-2 as #1)\n";
    cout << "  3) Best-of-3 short sets to 4 (TB7 at 4-4)\n";
}

static FormatConfig get_format_by_choice(int c) {
    FormatConfig fc;
    if (c==1) { fc.games_to_win_set=6; fc.tiebreak_at_games=6; fc.set_tiebreak_points=7; fc.deciding=DECIDING_REGULAR; fc.deciding_tb_points=10; }
    else if (c==2){ fc.games_to_win_set=6; fc.tiebreak_at_games=6; fc.set_tiebreak_points=7; fc.deciding=DECIDING_TB10; fc.deciding_tb_points=10; }
    else          { fc.games_to_win_set=4; fc.tiebreak_at_games=4; fc.set_tiebreak_points=7; fc.deciding=DECIDING_REGULAR; fc.deciding_tb_points=10; }
    return fc;
}

static void print_stats_menu() {
    cout << "\nStats Menu\n";
    cout << "  1) Match totals (choose player or both)\n";
    cout << "  2) By set (choose set, then player/both)\n";
    cout << "  3) Point-by-point log\n";
    cout << "  4) Back\n";
}

static void show_match_totals(const MatchState& st) {
    cout << "Show stats for: 1) " << st.player1_name
         << "  2) " << st.player2_name << "  3) Both\n";
    int c; cin>>c;
    if (c==1) print_single_player_stats(st.match_stats_p1, "== "+st.player1_name+" (Match Totals) ==");
    else if (c==2) print_single_player_stats(st.match_stats_p2, "== "+st.player2_name+" (Match Totals) ==");
    else if (c==3) print_side_by_side(st.match_stats_p1, st.match_stats_p2, st.player1_name, st.player2_name);
}

static void show_by_set(const MatchState& st) {
    cout << "Which set? (1-" << st.sets.size() << "): ";
    int s; cin>>s; if (s<1 || s>(int)st.sets.size()) return; int idx=s-1;
    cout << "Show stats for: 1) " << st.player1_name << "  2) " << st.player2_name << "  3) Both\n";
    int c; cin>>c;
    if (c==1) print_single_player_stats(st.per_set_stats_p1[idx], "== "+st.player1_name+" (Set "+to_string(s)+") ==");
    else if (c==2) print_single_player_stats(st.per_set_stats_p2[idx], "== "+st.player2_name+" (Set "+to_string(s)+") ==");
    else if (c==3) print_side_by_side(st.per_set_stats_p1[idx], st.per_set_stats_p2[idx], st.player1_name, st.player2_name);
}

static void show_point_by_point(const MatchState& st) {
    cout << "# | Set | Game | TB | Server | Serve | Winner | BP/GP/SP/MP | Event\n";
    for (size_t i=0;i<st.log_entries.size();i++) {
        const auto& e=st.log_entries[i];
        cout<<(i+1)<<" | "<<(e.set_index+1)<<" | "<<(e.game_index+1)<<" | "<<(e.in_tiebreak?"Y":"N")
            <<" | "<<(e.server_player==0?st.player1_name:st.player2_name)<<" | "
            <<(e.serve_type==SERVE_FIRST?"1st":(e.serve_type==SERVE_SECOND?"2nd":"-"))<<" | "
            <<(e.point_winner==0?st.player1_name:st.player2_name)<<" | "
            <<(e.was_break_point?"BP":"")<<(e.was_game_point?" GP":"")
            <<(e.was_set_point?" SP":"")<<(e.was_match_point?" MP":"")
            <<" | "<<e.event_chain<<"\n";
    }
}

// =============== Menus for point recording ===============

static void print_serve_menu() {
    cout << "\nServe/Event Menu:\n";
    cout << "  1) First serve in\n";
    cout << "  2) First serve fault -> second serve\n";
    cout << "  3) Second serve in\n";
    cout << "  4) Double fault\n";
    cout << "  5) Ace (first)\n";
    cout << "  6) Ace (second)\n";
    cout << "  7) Service winner (first)\n";
    cout << "  8) Service winner (second)\n";
    cout << "  9) Admin (stats/undo/end)\n";
}
static void print_return_menu() {
    cout << "\nReturn Menu:\n";
    cout << "  1) Return winner\n";
    cout << "  2) Return unforced error\n";
    cout << "  3) Return forced error\n";
    cout << "  4) Return in (go to rally)\n";
}
static void print_rally_menu() {
    cout << "\nRally Menu:\n";
    cout << "  1) Server winner\n";
    cout << "  2) Returner winner\n";
    cout << "  3) Server unforced error\n";
    cout << "  4) Returner unforced error\n";
    cout << "  5) Server forced error (drawn by returner)\n";
    cout << "  6) Returner forced error (drawn by server)\n";
}

// =============== Core point logic ===============

static void give_point_regular(MatchState& st, int winner_player) {
    if (winner_player==0) st.game_points_p1++; else st.game_points_p2++;
    int p1=st.game_points_p1, p2=st.game_points_p2;
    if (p1>=4 || p2>=4) {
        if (abs(p1-p2)>=2) {
            int game_winner = (p1>p2?0:1);
            award_game(st, game_winner);
            if (!st.in_match_tiebreak10 && check_enter_set_tiebreak(st)) {
                st.in_set_tiebreak=true;
                st.sets[st.current_set_index].set_tiebreak_played=true;
                st.tb_points_p1=0; st.tb_points_p2=0;
                // who serves first in set tiebreak? the next to serve — which is current_server now
                st.tb_start_server = st.current_server;
            }
            int set_winner=-1;
            if (!st.in_set_tiebreak) {
                if (set_is_won_now(st, set_winner)) {
                    close_set_and_prepare_next(st, set_winner);
                }
            }
        }
    }
}

static void give_point_tiebreak(MatchState& st, int winner_player, bool is_set_tb) {
    if (winner_player==0) st.tb_points_p1++; else st.tb_points_p2++;
    if (is_set_tb) {
        int set_winner=-1;
        if (set_is_won_now(st, set_winner)) {
            st.sets[st.current_set_index].tb_points_p1=st.tb_points_p1;
            st.sets[st.current_set_index].tb_points_p2=st.tb_points_p2;
            st.in_set_tiebreak=false;
            close_set_and_prepare_next(st, set_winner);
        }
    } else {
        int mw=-1;
        if (match_tiebreak10_won(st, mw)) {
            if (mw==0) st.sets_won_p1++; else st.sets_won_p2++;
            // add a final “set row” to display TB10 as a decider visually
            SetScore fin;
            fin.games_player1 = st.sets.back().games_player1;
            fin.games_player2 = st.sets.back().games_player2;
            fin.set_tiebreak_played = true;
            fin.tb_points_p1 = st.tb_points_p1;
            fin.tb_points_p2 = st.tb_points_p2;
            st.sets.push_back(fin);
            // inside match_tiebreak10_won(...) right after: st.sets.push_back(fin);
            st.per_set_stats_p1.push_back(PlayerStats());
            st.per_set_stats_p2.push_back(PlayerStats());
            st.in_match_tiebreak10=false;
        }
    }
}

static void record_point_and_stats(MatchState& st) {
    push_history(st);

    // If in tiebreak, enforce correct server for THIS point.
    if (st.in_set_tiebreak || st.in_match_tiebreak10) {
        compute_tiebreak_server(st);
    }

    // Flags before point (regular games only)
    bool was_break_point=false, was_game_point=false, was_set_point=false, was_match_point=false;
    if (!st.in_set_tiebreak && !st.in_match_tiebreak10) {
        was_break_point = is_break_point_if_receiver_wins(st);
        bool gp_p1 = is_game_point_for(st.game_points_p1, st.game_points_p2);
        bool gp_p2 = is_game_point_for(st.game_points_p2, st.game_points_p1);
        was_game_point = (gp_p1 || gp_p2);
        was_set_point = (is_set_point_if_player_wins(st,0) || is_set_point_if_player_wins(st,1));
        was_match_point = (is_match_point_if_player_wins(st,0) || is_match_point_if_player_wins(st,1));
    }

    PointLogEntry entry;
    entry.set_index = st.current_set_index;
    entry.game_index = st.sets[st.current_set_index].games_player1 + st.sets[st.current_set_index].games_player2;
    entry.in_tiebreak = (st.in_set_tiebreak || st.in_match_tiebreak10);
    entry.tiebreak_point_number = (st.tb_points_p1 + st.tb_points_p2 + 1);
    entry.point_number_in_game = (st.game_points_p1 + st.game_points_p2 + 1);
    entry.server_player = st.current_server;

    int server = st.current_server;
    int returner = (server==0?1:0);

    // ---- Serve/event menus ----
    while (true) {
        print_scoreboard(st);
        print_serve_menu();
        cout << "Choose: ";
        int c; cin>>c;

        if (c==9) {
            cout << "\nAdmin: 1) Stats  2) Undo last point  3) End match  4) Back\n";
            int a; cin>>a;
            if (a==1) {
                bool back=false;
                while(!back){
                    print_stats_menu();
                    int sm; cin>>sm;
                    if (sm==1) show_match_totals(st);
                    else if (sm==2) show_by_set(st);
                    else if (sm==3) show_point_by_point(st);
                    else back=true;
                }
            } else if (a==2) {
                pop_history(st); // remove our pre-push
                if (!pop_history(st)) cout<<"Nothing to undo.\n";
                else cout<<"Undid last point.\n";
                return;
            } else if (a==3) {
                pop_history(st);
                return;
            }
            continue;
        }

        if (c==1) { // 1st in
            st.current_point_serve = SERVE_FIRST;
            add_serve_attempt(st, server, SERVE_FIRST, true);
            entry.event_chain += "1st in; ";
            break;
        } else if (c==2) { // 1st fault -> second
            add_serve_attempt(st, server, SERVE_FIRST, false);
            entry.event_chain += "1st fault -> ";
            cout<<"Second serve: 1) in  2) double fault\n";
            int s2; cin>>s2;
            if (s2==1) {
                st.current_point_serve = SERVE_SECOND;
                add_serve_attempt(st, server, SERVE_SECOND, true);
                entry.event_chain += "2nd in; ";
                break;
            } else {
                st.current_point_serve = SERVE_SECOND;
                add_serve_attempt(st, server, SERVE_SECOND, false);
                add_double_fault(st, server);
                entry.event_chain += "double fault.";
                // Point to returner
                PlayerStats& mw = (returner==0? st.match_stats_p1 : st.match_stats_p2);
                PlayerStats& ml = (server==0? st.match_stats_p1 : st.match_stats_p2);
                add_stats_point_ownership(mw, ml);
                maybe_count_break_point(st, was_break_point, /*returner won*/true);
                entry.serve_type = SERVE_SECOND; entry.point_winner = returner;
                st.log_entries.push_back(entry);
                if (st.in_set_tiebreak) give_point_tiebreak(st, returner, true);
                else if (st.in_match_tiebreak10) give_point_tiebreak(st, returner, false);
                else give_point_regular(st, returner);
                return;
            }
        } else if (c==3) { // 2nd in
            st.current_point_serve = SERVE_SECOND;
            add_serve_attempt(st, server, SERVE_SECOND, true);
            entry.event_chain += "2nd in; ";
            break;
        } else if (c==4) { // DF
            st.current_point_serve = SERVE_SECOND;
            add_serve_attempt(st, server, SERVE_SECOND, false);
            add_double_fault(st, server);
            entry.event_chain += "double fault.";
            PlayerStats& mw = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (server==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, true);
            entry.serve_type=SERVE_SECOND; entry.point_winner=returner;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, returner, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, returner, false);
            else give_point_regular(st, returner);
            return;
        } else if (c==5) { // Ace 1st
            st.current_point_serve = SERVE_FIRST;
            add_serve_attempt(st, server, SERVE_FIRST, true);
            add_ace(st, server, SERVE_FIRST);
            add_server_point_won(st, server, SERVE_FIRST);
            entry.event_chain += "Ace (1st).";
            PlayerStats& mw = (server==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, false);
            entry.serve_type=SERVE_FIRST; entry.point_winner=server;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, server, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, server, false);
            else give_point_regular(st, server);
            return;
        } else if (c==6) { // Ace 2nd
            st.current_point_serve = SERVE_SECOND;
            add_serve_attempt(st, server, SERVE_SECOND, true);
            add_ace(st, server, SERVE_SECOND);
            add_server_point_won(st, server, SERVE_SECOND);
            entry.event_chain += "Ace (2nd).";
            PlayerStats& mw = (server==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, false);
            entry.serve_type=SERVE_SECOND; entry.point_winner=server;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, server, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, server, false);
            else give_point_regular(st, server);
            return;
        } else if (c==7) { // SW 1st
            st.current_point_serve = SERVE_FIRST;
            add_serve_attempt(st, server, SERVE_FIRST, true);
            add_service_winner(st, server, SERVE_FIRST);
            add_server_point_won(st, server, SERVE_FIRST);
            entry.event_chain += "Service winner (1st).";
            PlayerStats& mw = (server==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, false);
            entry.serve_type=SERVE_FIRST; entry.point_winner=server;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, server, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, server, false);
            else give_point_regular(st, server);
            return;
        } else if (c==8) { // SW 2nd
            st.current_point_serve = SERVE_SECOND;
            add_serve_attempt(st, server, SERVE_SECOND, true);
            add_service_winner(st, server, SERVE_SECOND);
            add_server_point_won(st, server, SERVE_SECOND);
            entry.event_chain += "Service winner (2nd).";
            PlayerStats& mw = (server==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, false);
            entry.serve_type=SERVE_SECOND; entry.point_winner=server;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, server, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, server, false);
            else give_point_regular(st, server);
            return;
        } else {
            cout<<"Invalid option.\n";
        }
    }

    // If serve is in, proceed to return/rally
    while (true) {
        print_scoreboard(st);
        print_return_menu();
        cout<<"Choose: ";
        int r; cin>>r;
        if (r==1) {
            add_return_outcome(st, returner, "winner");
            add_return_points_won(st, returner, st.current_point_serve);
            entry.event_chain += "Return winner.";
            PlayerStats& mw = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (server==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, true);
            entry.serve_type=st.current_point_serve; entry.point_winner=returner;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, returner, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, returner, false);
            else give_point_regular(st, returner);
            return;
        } else if (r==2) {
            add_return_outcome(st, returner, "ue");
            add_server_point_won(st, server, st.current_point_serve);
            entry.event_chain += "Return UE.";
            PlayerStats& mw = (server==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, false);
            entry.serve_type=st.current_point_serve; entry.point_winner=server;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, server, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, server, false);
            else give_point_regular(st, server);
            return;
        } else if (r==3) {
            add_return_outcome(st, returner, "fe");
            add_rally_outcome(st, server, "fedrawn");
            add_server_point_won(st, server, st.current_point_serve);
            entry.event_chain += "Return FE (drawn by server).";
            PlayerStats& mw = (server==0? st.match_stats_p1 : st.match_stats_p2);
            PlayerStats& ml = (returner==0? st.match_stats_p1 : st.match_stats_p2);
            add_stats_point_ownership(mw, ml);
            maybe_count_break_point(st, was_break_point, false);
            entry.serve_type=st.current_point_serve; entry.point_winner=server;
            st.log_entries.push_back(entry);
            if (st.in_set_tiebreak) give_point_tiebreak(st, server, true);
            else if (st.in_match_tiebreak10) give_point_tiebreak(st, server, false);
            else give_point_regular(st, server);
            return;
        } else if (r==4) {
            entry.event_chain += "Return in; ";
            break;
        } else {
            cout<<"Invalid option.\n";
        }
    }

    // Rally phase
    while (true) {
        print_scoreboard(st);
        print_rally_menu();
        cout<<"Choose: ";
        int rv; cin>>rv;

        cout<<"Mark net point? 1) No  2) Yes\n";
        int netChoice; cin>>netChoice;
        bool net_mark=(netChoice==2);
        int net_player=-1; bool net_won=false;
        if (net_mark) {
            cout<<"Who was at net? 1) "<<st.player1_name<<"  2) "<<st.player2_name<<"\n";
            int np; cin>>np; net_player=(np==1?0:1);
        }

        int point_winner=-1; string desc;
        if (rv==1){ add_rally_outcome(st, server, "winner"); add_server_point_won(st, server, st.current_point_serve); desc="Rally: server winner."; point_winner=server; }
        else if (rv==2){ add_rally_outcome(st, returner, "winner"); add_return_points_won(st, returner, st.current_point_serve); desc="Rally: returner winner."; point_winner=returner; }
        else if (rv==3){ add_rally_outcome(st, server, "ue"); add_return_points_won(st, returner, st.current_point_serve); desc="Rally: server UE."; point_winner=returner; }
        else if (rv==4){ add_rally_outcome(st, returner, "ue"); add_server_point_won(st, server, st.current_point_serve); desc="Rally: returner UE."; point_winner=server; }
        else if (rv==5){ add_rally_outcome(st, returner, "fedrawn"); add_return_points_won(st, returner, st.current_point_serve); desc="Rally: server FE (drawn by returner)."; point_winner=returner; }
        else if (rv==6){ add_rally_outcome(st, server, "fedrawn"); add_server_point_won(st, server, st.current_point_serve); desc="Rally: returner FE (drawn by server)."; point_winner=server; }
        else { cout<<"Invalid option.\n"; continue; }

        PlayerStats& mw = (point_winner==0? st.match_stats_p1 : st.match_stats_p2);
        PlayerStats& ml = (point_winner==0? st.match_stats_p2 : st.match_stats_p1);
        add_stats_point_ownership(mw, ml);

        if (net_mark) {
            if (net_player==0){ add_stats_net(st.match_stats_p1, point_winner==0); add_stats_net(st.per_set_stats_p1[st.current_set_index], point_winner==0); }
            else { add_stats_net(st.match_stats_p2, point_winner==1); add_stats_net(st.per_set_stats_p2[st.current_set_index], point_winner==1); }
        }

        bool returner_won=(point_winner==returner);
        maybe_count_break_point(st, was_break_point, returner_won);

        entry.event_chain += desc;
        entry.serve_type = st.current_point_serve;
        entry.point_winner = point_winner;
        st.log_entries.push_back(entry);

        if (st.in_set_tiebreak) give_point_tiebreak(st, point_winner, true);
        else if (st.in_match_tiebreak10) give_point_tiebreak(st, point_winner, false);
        else give_point_regular(st, point_winner);
        return;
    }
}

// =============== Main ===============

int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    MatchState st;

    cout<<"Enter Player 1 name: ";
    getline(cin, st.player1_name); if (st.player1_name.size()==0) getline(cin, st.player1_name);
    cout<<"Enter Player 2 name: ";
    getline(cin, st.player2_name); if (st.player2_name.size()==0) getline(cin, st.player2_name);
    cout<<"Enter Location (e.g., Club – Court #): ";
    getline(cin, st.location); if (st.location.size()==0) getline(cin, st.location);

    cout<<"Who serves first? 1) "<<st.player1_name<<"  2) "<<st.player2_name<<"\n";
    int sfirst=1; cin>>sfirst; st.current_server=(sfirst==2?1:0);

    print_format_menu();
    int fchoice=1; cin>>fchoice;
    st.format = get_format_by_choice(fchoice);

    // Start set 1
    start_new_set(st);

    bool done=false;
    while(!done){
        // If we are about to play a TB10 and have 0-0, ask for starting server once
        if (st.in_match_tiebreak10 && st.tb_points_p1==0 && st.tb_points_p2==0) {
            cout<<"Match TB10. Who serves first? 1) "<<st.player1_name<<"  2) "<<st.player2_name<<"\n";
            int tbsv; cin>>tbsv; st.tb_start_server=(tbsv==2?1:0);
            st.current_server = st.tb_start_server;
        }
        // If we just entered a set TB (set_tiebreak_played already true), tb_start_server already set to current_server at entry

        // In tiebreaks, recompute server each loop
        if (st.in_set_tiebreak || st.in_match_tiebreak10) {
            compute_tiebreak_server(st);
        }

        print_scoreboard(st);
        cout << "\nMain Menu:\n";
        cout << "  1) Record next point\n";
        cout << "  2) Stats menu\n";
        cout << "  3) Undo last point\n";
        cout << "  4) End match (finish now)\n";
        cout << "Choose: ";
        int m; cin>>m;

        if (m==1) {
            record_point_and_stats(st);

            // If in TB, server will be recomputed next loop. If a set ended or TB10 ended,
            // close_set_and_prepare_next or the TB10 checker already handled transitions.

            if (match_is_over_now(st)) {
                print_scoreboard(st);
                cout<<"\nMatch finished!\n";
                cout<<"Final sets won: "<<st.player1_name<<" "<<st.sets_won_p1<<" - "<<st.player2_name<<" "<<st.sets_won_p2<<"\n";
                cout<<"Final set scores:\n";
                for (size_t i=0;i<st.sets.size();i++) {
                    const auto& s=st.sets[i];
                    cout<<"  Set "<<(i+1)<<": "<<s.games_player1<<"-"<<s.games_player2;
                    if (s.set_tiebreak_played) cout<<" (TB "<<s.tb_points_p1<<"-"<<s.tb_points_p2<<")";
                    cout<<"\n";
                }
                cout<<"\nShow stats? 1) "<<st.player1_name<<"  2) "<<st.player2_name<<"  3) Both  4) Save results  5) Exit\n";
                int e; cin>>e;
                if (e==1) print_single_player_stats(st.match_stats_p1, "== "+st.player1_name+" (Match Totals) ==");
                else if (e==2) print_single_player_stats(st.match_stats_p2, "== "+st.player2_name+" (Match Totals) ==");
                else if (e==3) print_side_by_side(st.match_stats_p1, st.match_stats_p2, st.player1_name, st.player2_name);
                else if (e==4) save_match_files(st);
                done=true;
            }

        } else if (m==2) {
            bool back=false;
            while(!back){
                print_scoreboard(st);
                print_stats_menu();
                int sm; cin>>sm;
                if (sm==1) show_match_totals(st);
                else if (sm==2) show_by_set(st);
                else if (sm==3) show_point_by_point(st);
                else back=true;
            }

        } else if (m==3) {
            if (!pop_history(st)) cout<<"Nothing to undo.\n";
            else cout<<"Undid last point.\n";

        } else if (m==4) {
            cout<<"End match now. Show stats? 1) "<<st.player1_name<<"  2) "<<st.player2_name
                <<"  3) Both  4) Save results  5) Exit\n";
            int e; cin>>e;
            if (e==1) print_single_player_stats(st.match_stats_p1, "== "+st.player1_name+" (Totals so far) ==");
            else if (e==2) print_single_player_stats(st.match_stats_p2, "== "+st.player2_name+" (Totals so far) ==");
            else if (e==3) print_side_by_side(st.match_stats_p1, st.match_stats_p2, st.player1_name, st.player2_name);
            else if (e==4) save_match_files(st);
            done=true;
        } else {
            cout<<"Invalid option.\n";
        }
    }

    cout<<"Goodbye.\n";
    return 0;
}
