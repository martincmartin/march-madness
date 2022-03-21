// To compile and run:
//
// brew install curl nlohmann-json fmt
// Use "brew --prefix <package>" to find where they're installed.  For me:
//
// clang++ -std=gnu++20 -O3 -DNDEBUG -Wall -Werror
// -I/opt/homebrew/opt/fmt/include -L/opt/homebrew/opt/fmt/lib
// -I/opt/homebrew/opt/curl/include -L/opt/homebrew/opt/curl/lib
// -I/opt/homebrew/opt/nlohmann-json/include -lcurl -lfmt main.cpp && time
// ./a.out

// NOTE: Match, matchup and game are used interchangeably in this code.  Should
// probably standardize on match.  Oh well.

// This code runs in reasonable time & RAM after round of 64 / before
// round-of-32.  However, mid round of 64, it's taking 15.6 GB on Ubuntu and
// swapping, and 21 minutes, and is killed just before finishing. And that's the
// version that only computes score tuples, not probabilities.  So still not
// practical.  Could always do Monte Carlo, or trim unlikley outcomes I suppose.
// Actually, could do Monte Carlo just for round 0!  That would solve all
// possibilities.

// I think we should get our forecast from 538.  They update them all the
// time, so e.g. after round of 64 I can get fresh probabilities.  Ken Pomeroy
// sometimes updates his, but sometimes doesn't, even after first four.  Plus I
// don't think he ever updates them between rounds of 64 and 32.  URL:
// https://projects.fivethirtyeight.com/march-madness-api/2022/fivethirtyeight_ncaa_forecasts.csv

#include <cstdlib>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <bitset>
#include <unordered_set>
#include <bit>
#include <curl/curl.h>
#include <chrono>

// constexpr const char *WHEN_RUN = "mid2-roundof32";
constexpr const char *WHEN_RUN = "before-sweet16";

#define YEAR "2022"

constexpr const char *PROBS_FNAME = YEAR "/{}-fivethirtyeight_ncaa_forecasts.csv";

constexpr size_t NUM_TEAMS = 64;
constexpr size_t NUM_GAMES = NUM_TEAMS - 1;
constexpr size_t NUM_ROUNDS = 6;

using game_t = int_fast8_t;
using team_t = int_fast8_t;

using namespace std;

using namespace std::chrono;
using json = nlohmann::json;

constexpr size_t NUM_BRACKETS = 8;

array<uint64_t, NUM_BRACKETS> entries{
    60122219, // me, Hoops, There It Is! (Martin, martinisquared)
    62328104, // Tara, TheGambler46
    58407997, // Dan (Dan Murphy, dmurph888)
    58468455, // Eileen-er-iffic (Eileen (Dan's Sister), The_MRF_NYC)
    61439241, // Vakidis (Billy (Maureen's Husband), wgarbarini)
    62484411, // Owe'n Charlie '22 (Uncle Dennis, tiger72pu)
    65481077, // Maureen's Annual Bonus (Joe (Eileen's Husband), gettinpiggywitit)
    69629125, // RPcatsmounts! espn88461517
              // 61783453,  # Villa-Mo-va 1 (Maureen (Dan's Sister), Villa-Mo-va)
};

void assert_failed(const char *expr)
{
   fprintf(stderr, "Assertion failed: %s\n", expr);
   abort();
}

#define myassert(expr) ((expr) ? (void)0 : assert_failed(#expr))

vector<string> split(string source, char delim)
{
   vector<string> result;
   stringstream stream(source);
   string item;
   while (getline(stream, item, delim))
   {
      result.push_back(item);
   }
   return result;
}

/**********  Fetch a URL, with caching.  **********/
size_t write_callback(char *buffer, size_t size, size_t nmemb, void *user_data)
{
   stringstream &stream = *reinterpret_cast<stringstream *>(user_data);

   stream.write((const char *)buffer, size * nmemb);

   return size * nmemb;
}

std::string get_url(const string &url)
{
   CURL *curl = curl_easy_init();
   if (!curl)
   {
      throw runtime_error("curl_easy_init() failed.");
   }

   cout << "Fetching " << url << endl;

   stringstream stream;

   curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);

   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK)
   {
      curl_easy_cleanup(curl);
      throw runtime_error(curl_easy_strerror(res));
   }

   long http_code{0};
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   if (http_code != 200)
   {
      curl_easy_cleanup(curl);
      throw runtime_error("Didn't get 200.");
   }

   curl_easy_cleanup(curl);

   return stream.str();
}

string get_with_caching(string url, string fpath)
{
   {
      ifstream myinfile(fpath);
      if (myinfile)
      {
         stringstream stream;
         // TODO: How do I check for errors while reading?
         stream << myinfile.rdbuf();
         return stream.str();
      }
   }

   string body = get_url(url);
   ofstream myoutfile(fpath);
   if (!myoutfile)
   {
      throw runtime_error("Error opening file to write " + fpath);
   }

   myoutfile << body;

   return body;
}

/**********  Read forecasts from CSV file  **********/

// There doesn't seem to be a standard CSV parsing library in C++.  The answer
// seems to be, just use ifstream and getline(), i.e. ignore quotes and
// escaping.  If you need something better, consider rapidcsv from here:
// https://github.com/d99kris/rapidcsv
//
// The "rd?_win" columns seem to be the probability of winning that round.  rd1
// is the "First Four", where we narrow from 68 to 64.  rd2 is the Round of 64,
// rd3 is Round of 32.  Just keep the first one you see, i.e. assume the most
// recent entries are on top.
//
// What a pain in the ass to parse all this.

vector<string> teams(NUM_TEAMS);
unordered_map<int, team_t> eid_to_team;

// vector<string> teams_normalized(NUM_TEAMS);

/*
void replace(string &original, const char *old_str, const char *new_str)
{
   size_t pos = original.find(old_str);
   if (pos != string::npos)
   {
      original.replace(pos, strlen(old_str), new_str);
   }
}

void normalize(string &team_name)
{
   for (auto &character : team_name)
   {
      character = tolower(character);
   }

   replace(team_name, "state", "st");
   replace(team_name, " (ca)", "");
   replace(team_name, "texas christian", "tcu");
   replace(team_name, " (fl)", "");
   replace(team_name, "j'ville", "jacksonville");
   replace(team_name, "connecticut", "uconn");
}
*/

struct CSVFile
{
   vector<string> headers;
   vector<vector<string>> rows;

   int column(const string &name)
   {
      return find(headers.begin(), headers.end(), name) - headers.begin();
   }
};

string get_forecasts()
{
   return get_with_caching("https://projects.fivethirtyeight.com/march-madness-api/2022/fivethirtyeight_ncaa_forecasts.csv",
                           fmt::format(PROBS_FNAME, WHEN_RUN));
}

CSVFile parse_csv()
{
   CSVFile result;
   stringstream mystream(get_forecasts());

   string line;
   // Read header.
   if (!getline(mystream, line))
   {
      throw runtime_error("Failed to read header from forecasts CSV file.");
   }
   result.headers = split(line, ',');
   while (getline(mystream, line))
   {
      result.rows.push_back(split(line, ','));
   }
   return result;
}

vector<array<double, NUM_ROUNDS>> probs(NUM_TEAMS);

void parse_probs()
{
   CSVFile csv = parse_csv();

   int gender = csv.column("gender");
   int name = csv.column("team_name");
   int id = csv.column("team_id");
   int playin = csv.column("playin_flag");
   int rd2 = csv.column("rd2_win");

   vector<bool> seen(NUM_TEAMS);

   for (auto &row : csv.rows)
   {
      if (row[gender] != "mens")
      {
         continue;
      }

      auto iter = eid_to_team.find(stoi(row[id]));

      if (iter == eid_to_team.end())
      {
         // Let's ignore unrecognized teams from the first four.
         if (row[playin] == "0")
         {
            cout << "Team " << row[name] << " eid " << row[id] << " from probability CSV not recognized.\n";
            abort();
         }
         continue;
      }

      int team_id = iter->second;

      array<double, NUM_ROUNDS> &this_probs = probs[team_id];
      if (!seen[team_id])
      {
         seen[team_id] = true;
         double prev_prob;
         for (size_t round = 0; round < NUM_ROUNDS; ++round)
         {
            double this_prob = stod(row[rd2 + round]);
            if (round == 0 || this_prob == 0.0)
            {
               this_probs[round] = this_prob;
            }
            else
            {
               this_probs[round] = this_prob / prev_prob;
            }

            prev_prob = this_prob;
         }
      }
   }
}

double get_prob(team_t team_index, int round)
{
   return probs[team_index][NUM_ROUNDS - 1 - round];
}

double game_prob(team_t first, team_t second, team_t winner, int round)
{
   myassert(winner >= 0);
   myassert(first >= 0);
   myassert(second >= 0);
   myassert(winner == first || winner == second);

   double result = get_prob(winner, round) / (get_prob(first, round) + get_prob(second, round));
   if (isnan(result))
   {
      cout << "first: " << first << ", second: " << second << ", winner: " << winner << ", round: " << round << endl;
      cout << get_prob(winner, round) << endl;
      cout << get_prob(first, round) << endl;
      cout << get_prob(second, round) << endl;
   }
   myassert(!isnan(result));
   return result;
}

/**********  Fetch a bracket, extract & parse JSON  **********/

string URL_FORMAT = "https://fantasy.espn.com/tournament-challenge-bracket/" YEAR
                    "/en/entry?entryID={}";

string get_entry(uint64_t entry)
{
   return get_with_caching(
       fmt::format(URL_FORMAT, entry),
       fmt::format(YEAR "/pages/{}-{}.html", entry, WHEN_RUN));
}

json get_json(const string &var, const string &source)
{
   regex myregex(var + R"(\s*=\s*(.+);)"
                       "\n");
   smatch mymatch;
   regex_search(source, mymatch, myregex);
   string raw = mymatch[1].str();
   if (raw[0] == '\'')
   {
      myassert(raw[raw.size() - 1] == '\'');
      raw[0] = '"';
      raw[raw.size() - 1] = '"';
   }
   return json::parse(raw);
}

/**********  Manipulate indexes of matches  **********/

struct Round
{
   int round;
   int num_teams;
   int num_matches;
   int index;
};

inline Round round_index(int match)
{
   myassert(1 <= match && match <= NUM_GAMES);
   // For x > 0, bit_width(x) == 1 + floor(log2(x)).
   int round = bit_width((uint8_t)(64 - match)) - 1;
   int num_matches = (1 << round);
   int num_teams = num_matches * 2;
   return Round{round, num_teams, num_matches, num_teams - (64 - match)};
}

// Index is one based, i.e. goes from 1 .. 32 for Round Of 64.
int match(int round, int index)
{
   assert(index >= 1);
   return index + 64 - (1 << (round + 1));
}

int input(int index)
{
   Round round_details = round_index(index + 1);
   int inputRound = round_details.round + 1; // + 1 for previous round.
   int inputRoundIndex = round_details.index * 2 - 1;
   return match(inputRound, inputRoundIndex) - 1;
}

array<int, NUM_GAMES> points_per_match{
    // Round of 64
    10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10,
    // Round of 32
    20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20,
    // Sweet 16
    40, 40, 40, 40, 40, 40, 40, 40,
    // Elite 8
    80, 80, 80, 80,
    // Final 4
    160, 160,
    // Championship
    320};

/**********  Tuples of scores, packed in an int  **********/

// This should probably be a simple class, rather than a typedef.  Oh well.
using scoretuple_t = uint64_t; // For more than 8 players, need uint128_t.
static_assert(sizeof(scoretuple_t) >= NUM_BRACKETS);

int winner(scoretuple_t scores)
{
   const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&scores);

   uint8_t biggest = bytes[0];
   int index = 0;
   for (size_t i = 1; i < NUM_BRACKETS; i++)
   {
      if (bytes[i] > biggest)
      {
         biggest = bytes[i];
         index = i;
      }
   }

   return index;
}

scoretuple_t normalize(scoretuple_t input)
{
   scoretuple_t result = input;
   uint8_t *bytes = reinterpret_cast<uint8_t *>(&result);
   uint8_t smallest = bytes[0];
   for (size_t i = 1; i < NUM_BRACKETS; i++)
   {
      if (bytes[i] < smallest)
      {
         smallest = bytes[i];
      }
   }

   for (size_t i = 0; i < NUM_BRACKETS; i++)
   {
      bytes[i] -= smallest;
   }

   return result;
}

string make_string(scoretuple_t scores)
{
   const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&scores);
   string result = "(";
   bool first = true;
   for (size_t i = 0; i < NUM_BRACKETS; i++)
   {
      if (!first)
      {
         result += ", ";
      }
      result += to_string(bytes[i] * 10);
      first = false;
   }
   return result + ")";
}

/**********  Team, match, bracket  **********/

struct Matchup
{
   game_t id = -1;
   team_t first_team = -1; // Should really use std::optional for these.
   team_t second_team = -1;
   team_t winner = -1;
};

string to_string(const Matchup &matchup)
{
   return to_string(matchup.id) + ": " +
          teams[matchup.first_team] + ", " + teams[matchup.second_team] + ", " +
          (matchup.winner >= 0 ? teams[matchup.winner] : "None");
}

Matchup parse_matchup(const json &matchup)
{
   Matchup result;
   result.id = matchup["id"].get<int>() - 63;
   const auto &match_teams = matchup["o"];
   if (match_teams.size() >= 1)
   {
      result.first_team = match_teams[0]["id"].get<int>() - 65;
      myassert(teams[result.first_team] == match_teams[0]["n"]);
   }
   if (match_teams.size() >= 2)
   {
      result.second_team = match_teams[1]["id"].get<int>() - 65;
      myassert(teams[result.second_team] == match_teams[1]["n"]);
   }
   if (matchup.find("w") != matchup.end())
   {
      result.winner = matchup["w"].get<int>() - 65;
   }
   return result;
}

vector<Matchup> games(NUM_GAMES);

struct Bracket
{
   string name;
   vector<team_t> picks;
};

vector<Bracket> brackets;

vector<bitset<NUM_TEAMS>> all_selections(NUM_GAMES);

string to_string(const bitset<NUM_TEAMS> selections)
{
   string result;
   bool first = true;
   for (size_t i = 0; i < selections.size(); ++i)
   {
      if (selections[i])
      {
         if (!first)
         {
            result += ", ";
         }
         result += teams[i];
         first = false;
      }
   }
   return result;
}

Bracket get_bracket(uint64_t entry)
{
   Bracket result;
   string html = get_entry(entry);
   result.name = get_json("espn.fantasy.maxpart.config.Entry", html)["n_e"];
   // Get rid of some Unicode.
   if (result.name.starts_with("Owe"))
   {
      result.name = "Owe'n Charlie '22";
   }
   if (result.name.starts_with("Maureen"))
   {
      result.name = "Maureen's Annual Bonus";
   }

   auto picks_str = get_json("espn.fantasy.maxpart.config.pickString", html);
   for (auto &pick_str : split(picks_str.get<string>(), '|'))
   {
      result.picks.push_back(stoi(pick_str) - 1);
   }
   myassert(result.picks.size() == NUM_GAMES);

   return result;
}

void make_all_selections()
{
   for (int i = 0; i < NUM_GAMES; i++)
   {
      for (const auto &bracket : brackets)
      {
         all_selections[i].set(bracket.picks[i]);
      }
   }
}

/**********  Boolean Expressions, for "paths of glory"  **********/

class BoolExpr
{
public:
   virtual ~BoolExpr() {}
};

struct Var : public BoolExpr
{
   string name;
   size_t index;
   bool first;

   static vector<pair<shared_ptr<Var>, shared_ptr<Var>>> all_vars;
};

vector<pair<shared_ptr<Var>, shared_ptr<Var>>> Var::all_vars(NUM_TEAMS);

class OrExpr : public BoolExpr
{
public:
   unordered_set<shared_ptr<BoolExpr>> children;

   void add(shared_ptr<BoolExpr> child)
   {
      if (children.find(child) != children.end())
      {
         return;
      }
      if (auto var = reinterpret_cast<Var *>(child.get()))
      {
         // Find it's negated twin.
         auto &temp = Var::all_vars[var->index];
         myassert((var->first ? temp.first : temp.second).get() == var);
         shared_ptr<Var> &twin = var->first ? temp.second : temp.first;
         if (children.find(twin) != children.end())
         {
            // We have "P or not P", which is always true, so replace ourself
            // with "true", which is just OrExpr of the empty set.
            children.clear();
            // If parent is AndExpr, we should get rid of this node entirely.  Hmm.
            return;
         }
      }
      children.insert(child);
      // If child is AndExpr, and has a var (or subexression?) in common with
      // existing child, factor them out: PQ \/ PR = P(Q \/ R)
   }
};

class AndExpr : public BoolExpr
{
public:
   unordered_set<shared_ptr<BoolExpr>> children;

   void add(shared_ptr<BoolExpr> child)
   {
      children.insert(child);
   }
};

/**********  Outcomes  **********/

scoretuple_t
get_scoretuple(game_t match_index, team_t winning_team, uint8_t reduced_points)
{
   scoretuple_t scores{0};

   uint8_t *bytes = reinterpret_cast<uint8_t *>(&scores);

   for (size_t i = 0; i < brackets.size(); i++)
   {
      if (brackets[i].picks[match_index] == winning_team)
      {
         bytes[i] = reduced_points;
      }
   }

   return scores;
}

struct Outcomes
{
   team_t team;
   unordered_map<scoretuple_t, double> scores;

   Outcomes(team_t team, scoretuple_t scores, double prob) : team(team), scores{{scores, prob}} {}
   Outcomes() : team(-1) {}
   Outcomes(team_t team) : team(team) {}
};

string to_string(const Outcomes &outcome)
{
   string result;
   result += (outcome.team < 0 ? "other" : teams[outcome.team]) + ":";
   for (const auto scores : outcome.scores)
   {
      result += " " + make_string(scores.first) + " (" + to_string(scores.second) + ")";
   }
   return result + "\n";
}

Outcomes *find_team(vector<Outcomes> &outcomes, team_t team)
{
   for (auto &outcome : outcomes)
   {
      if (outcome.team == team)
      {
         return &outcome;
      }
   }
   return nullptr;
}

struct TeamWithProb
{
   team_t team;
   double prob;
};

// The first element of the vector is always for team "other".
vector<Outcomes>
outcomes(
    game_t match_index,
    bitset<64> selections)
{
   const Matchup &game = games[match_index];
   const auto ri = round_index(match_index + 1);
   int this_points = points_per_match[match_index];
   if (ri.round == 5)
   {
      // Base case, round of 64.
      myassert(this_points == 10);
      vector<Outcomes> result(1);
      vector<TeamWithProb> teams_with_probs;
      if (game.winner >= 0)
      {
         teams_with_probs.push_back({game.winner, 1.0});
      }
      else
      {
         myassert(game.first_team >= 0);
         myassert(game.second_team >= 0);
         double prob_first = game_prob(game.first_team, game.second_team, game.first_team, ri.round);
         teams_with_probs.push_back({game.first_team, prob_first});
         teams_with_probs.push_back({game.second_team, 1.0 - prob_first});
      }

      for (const TeamWithProb &team_with_prob : teams_with_probs)
      {
         auto scores = get_scoretuple(match_index, team_with_prob.team, this_points / 10);
         myassert(team_with_prob.team >= 0);

         if (true /* selections[team_with_prob.team] */)
         {
            result.emplace_back(team_with_prob.team, scores, team_with_prob.prob);
         }
         else
         {
            result[0].scores[scores] += team_with_prob.prob;
         }
      }

      return result;
   }

   // General case.  Start by recursing.
   int prev_match = input(match_index);
   const auto outcomes1 = outcomes(prev_match, all_selections[match_index]);
   const auto outcomes2 = outcomes(prev_match + 1, all_selections[match_index]);
   vector<Outcomes> result(1);
   if (ri.round <= 1)
   {
      cout << "About to loop, round " << ri.round << endl;
   }

   for (const auto &outcome1 : outcomes1)
   {
      if (outcome1.scores.empty())
      {
         continue;
      }
      myassert(outcome1.team >= 0);
      /*
      if (ri.round == 0)
      {
         cout << (outcome1.team < 0 ? "other" : teams[outcome1.team]) << endl;
      }
      */
      for (const auto &outcome2 : outcomes2)
      {
         if (outcome2.scores.empty())
         {
            continue;
         }
         myassert(outcome2.team >= 0);

         /*
         if (ri.round == 0)
         {
            cout << "    " << (outcome2.team < 0 ? "other" : teams[outcome2.team]) << endl;
         }
         */

         vector<TeamWithProb> teams_with_probs;
         if (game.winner >= 0)
         {
            // If this game has been played in real life, then all previous games
            // have also been played, so our recursive outcomes had better have
            // only a single non-empty result.
            myassert(outcomes1.size() == 1 || (outcomes1.size() == 2 && outcomes1[0].scores.empty()));
            myassert(outcomes2.size() == 1 || (outcomes2.size() == 2 && outcomes2[0].scores.empty()));
            myassert(game.winner == outcome1.team || game.winner == outcome2.team);
            teams_with_probs.push_back({game.winner, 1.0});
         }
         else
         {
            double prob_first = game_prob(outcome1.team, outcome2.team, outcome1.team, ri.round);
            teams_with_probs.push_back({outcome1.team, prob_first});
            teams_with_probs.push_back({outcome2.team, 1.0 - prob_first});
         }

         // So if both are "other", do we even need to loop over two winners?
         // Since the scores will be exactly the same either way?
         for (const auto &winner : teams_with_probs)
         {
            myassert(winner.team >= 0);
            // Find the destination spot in result
            Outcomes *dest;
            if (false /* winner < 0 || !selections[winner] */)
            {
               dest = &result[0];
            }
            else
            {
               dest = find_team(result, winner.team);
               if (!dest)
               {
                  result.emplace_back(winner.team);
                  dest = &result[result.size() - 1];
               }
            }

            for (const auto score_and_prob1 : outcome1.scores)
            {
               for (const auto score_and_prob2 : outcome2.scores)
               {
                  scoretuple_t total_scores = score_and_prob1.first + score_and_prob2.first;
                  scoretuple_t overall_scores;
                  if (winner.team < 0)
                  {
                     overall_scores = total_scores;
                  }
                  else
                  {
                     auto this_scores = get_scoretuple(match_index, winner.team, this_points / 10);
                     overall_scores = total_scores + this_scores;
                  }
                  dest->scores[normalize(overall_scores)] += winner.prob * score_and_prob1.second * score_and_prob2.second;
               }
            }
         }
      }
   }

   if (ri.round <= 1)
   {
      cout << "loop done.\n";
   }
   return result;
}

template <typename Clock>
double elapsed(time_point<Clock> start, time_point<Clock> end)
{
   return duration_cast<microseconds>(end - start).count() / 1e6;
}

/**********  Putting it all together  **********/

int main(int argc, char *argv[])
{
   string html = get_entry(entries[0]);
   const auto teams_json = get_json("espn.fantasy.maxpart.config.scoreboard_teams", html);
   myassert(teams_json.size() == NUM_TEAMS);
   for (const auto &team : teams_json)
   {
      int id = team["id"].get<int>() - 1;
      teams[id] = team["n"];
      eid_to_team[team["eid"].get<int>()] = id;
   }

   const auto matchups_json = get_json("espn.fantasy.maxpart.config.scoreboard_matchups", html);

   myassert(matchups_json.size() == NUM_GAMES);
   for (const auto &matchup : matchups_json)
   {
      Matchup m = parse_matchup(matchup);
      games[m.id] = m;
   }

   for (auto entry : entries)
   {
      brackets.push_back(get_bracket(entry));
   }

   myassert(brackets.size() == NUM_BRACKETS);

   make_all_selections();

   myassert(all_selections.size() == NUM_GAMES);

   parse_probs();

   {
      cout << "**********  2nd from the top Round of 32 game in the South\n";
      auto result = outcomes(41, all_selections[41]);
      for (const auto &outcome : result)
      {
         if (!outcome.scores.empty())
         {
            cout << to_string(outcome);
         }
      }
      /* Correct results for before-roundof64:
      Houston: (30, 30, 10, 10, 30, 10, 0, 30) (0.104051) (40, 40, 20, 20, 40, 0, 10, 40) (0.332539)
      Illinois: (0, 0, 20, 20, 0, 0, 10, 0) (0.109205) (20, 20, 40, 40, 20, 0, 10, 20) (0.286999)
      Chattanooga: (0, 0, 0, 0, 0, 40, 10, 0) (0.018375) (10, 10, 10, 10, 10, 30, 0, 10) (0.044459)
      UAB: (0, 0, 0, 0, 0, 20, 30, 0) (0.026475) (0, 0, 0, 0, 0, 0, 30, 0) (0.077896)
      */
   }

   {
      cout << "**********  Top Sweet 16 in the South\n";
      auto result = outcomes(52, all_selections[52]);
      for (const auto &outcome : result)
      {
         if (!outcome.scores.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   {
      cout << "**********  Wisconsin vs COLG\n";
      auto result = outcomes(29, all_selections[29]);
      for (const auto &outcome : result)
      {
         if (!outcome.scores.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   {
      cout << "**********  West & East\n";
      auto west_east = outcomes(60, all_selections[62]);
      for (const auto &outcome : west_east)
      {
         if (!outcome.scores.empty())
         {
            cout << (outcome.team < 0 ? "other" : teams[outcome.team]) << ": " << outcome.scores.size() << "\n";
         }
      }
   }

   {
      cout << "**********  Midwest & South\n";
      auto midwest_south = outcomes(61, all_selections[62]);
      for (const auto &outcome : midwest_south)
      {
         if (!outcome.scores.empty())
         {
            cout << (outcome.team < 0 ? "other" : teams[outcome.team]) << ": " << outcome.scores.size() << "\n";
         }
      }
   }

   {
      cout << "**********  Whole Thing!\n";
      auto start = high_resolution_clock::now();
      auto results = outcomes(62, {});
      cout << "Elapsed " << elapsed(start, high_resolution_clock::now()) << " sec.\n";
      vector<pair<int, double>> win_probs(NUM_BRACKETS);
      for (size_t i = 0; i < NUM_BRACKETS; ++i)
      {
         win_probs[i].first = i;
      }

      for (const Outcomes &outc : results)
      {
         for (const auto &score_and_prob : outc.scores)
         {
            win_probs[winner(score_and_prob.first)].second += score_and_prob.second;
         }
      }

      cout << "***** Probability of Win for each Bracket *****\n";
      sort(win_probs.begin(), win_probs.end(), [](auto &a, auto &b)
           { return a.second > b.second; });
      for (int i = 0; i < NUM_BRACKETS; ++i)
      {
         cout << fmt::format("{:<22}: {:5.2f}%\n", brackets[win_probs[i].first].name, win_probs[i].second * 100);
      }
   }

   return 0;
}
