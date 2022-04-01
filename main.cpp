// To compile and run:
//
// brew install curl nlohmann-json fmt
// Use "brew --prefix <package>" to find where they're installed.  For me:
//
// clang++ -std=gnu++20 -O3 -Wall -Werror
// -I/opt/homebrew/opt/fmt/include -L/opt/homebrew/opt/fmt/lib
// -I/opt/homebrew/opt/curl/include -L/opt/homebrew/opt/curl/lib
// -I/opt/homebrew/opt/nlohmann-json/include -lcurl -lfmt main.cpp && time
// ./a.out

// **********  TODO next year (2023)  **********
//
// Optimizing: start with 538's choices, get single & double optimzing working.
// Then implement the "all 2^n combinations downstream from a given choice"
// optimizing.

// Write  blog post: you root for your team, but even if they win it may not
// help if others chose the same thing.  Figure out probability of winning and
// paths of glory.  Doesn't scale to round of 32 or round of 64.  But you don't
// need full 2^n.  Instead, you need score tuples & winner.  That gets you to
// round of 32.  Round of 64 is just two independent Round of 32s, with a
// championship.  So can do Monte Carlo for the championship but exact before
// that.  Less variance, and therefore should require fewer Monte Carlo
// iterations and be overall faster.

// Paths of glory: The boolean stuff turned out to be a bust.  Automate choosing
// matches_to_consider, see the TODO comment above it.  Could also look more
// games in the future, i.e. not just "must win" games in the next round, but
// also the round after that.  Paths of Glory are probably too complicated until
// at least the round of 32 is over.

// **********  Notes from optimizing  **********
// In 2022: Best bracket I could find just choose most likely winner for round
// of 64 and round of 32, i.e. just what 538 predicted, never needed to look at
// other brackets in my group.
//
// You won't find the best by just hill climbing with single changes:
// considering all pairs of changes produced a slightly better result than just
// only considering single flips.  However, the improvement just meant winning
// one more year every century.
//
// One strategy perhaps worth trying: when you consider a change, also consider
// all games downstream of it.  So when you consider flipping a Sweet 16 choice,
// evaluate all combinations of flipping Elite 8, Final 4 and championship.
// That's 8 possibilities per Sweet 16 choice.  If we did that for all Sweet 16
// single flips, that's only 8 * 8 = 64 simulations, same as single_optimize().
// Might be better use of limited simulations than double_optimize().
//
// If you try it at Round of 64, that's 32 * 32 = 1024, still faster than
// double_optimize(), which took almost 3 hours on Crystal (Ubuntu).

// **********  Other Notes  **********

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

// Well, the problem of finding a minimal boolean expression for a given truth
// table is NP-hard.

// Well, I can simplify it a little more.  The boolean expressions that come out
// of the championship, for the unique score tuples but before reducing over
// individual winners, have expressions of the form (A or (notA and B)), these
// can be simplified to (A or B).  (And the dual, (A and (notA or B)) ==
// (A and B)).
//
// My other thought is to look for "must win" games, and also pairs and
// tripples.  That is, "if this game goes the wrong way I'm eliminated, but if
// it goes the right way, I may or may not win."  The intuition being, if a
// minimal boolean expression is more complicated than that, it's probably hard
// to understand or interpret anyway.
//
// Current status, as of 10 hours before Sweet 16 starts in 2022:
//
// I'm not 100% sure the boolean simplifier is correct.  But it does omptimally
// simplify expressions, i.e. produce minimal expressions, all the way up to the
// championship, before we reduce over individual winners.  Once we start
// reducing over individual winners, the expressions get wicked complicated and
// it's not clear if this is because (a) there's a bug, (b) it's missing some
// opportunity for simplifying, or (c) the reality is just really complicated
// and simplifying is a fools errand that won't provide insight.
//
// So we have about 4500 unique score tuples coming out of the championship,
// each with minimal bool expressions.
//
// On question to look at: in the next round, which outcome changes my chance of
// winning the most?  Which pair or triple of outcomes?

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
#include <fcntl.h>
#include <unistd.h>

#define WITH_BOOLEXPR 0

#if WITH_BOOLEXPR
#define BOOLEXPR(expr) expr
#else
#define BOOLEXPR(expr)
#endif

#define COMMA ,

// constexpr const char *WHEN_RUN = "before-final4";
constexpr const char *WHEN_RUN = "before-roundof64";

#define YEAR "2022"

constexpr const char *PROBS_FNAME = YEAR "/{}-fivethirtyeight_ncaa_forecasts.csv";

using game_t = int_fast8_t;
using team_t = int_fast8_t;

constexpr team_t NUM_TEAMS = 64;
constexpr game_t NUM_GAMES = NUM_TEAMS - 1;
constexpr size_t NUM_ROUNDS = 6;

// In 2022, before-roundof64 had ~ 16M in South-Midwest, which my Mackbook Air
// could do in 1.6 seconds.  So we want the threshold higher than that.
constexpr size_t MONTE_CARLO_THRESHOLD = 100'000'000;
// 100,000,000 iters runs out of RAM on Mac Airbook (8 GB).
constexpr size_t MONTE_CARLO_ITERS = 1'000'000;

using namespace std;

using namespace std::chrono;
using json = nlohmann::json;

constexpr size_t NUM_BRACKETS = 8;

// Summary: lots of Villanova fans.  In 2022, optimizer chose Villanova to make
// it to the championship.
array<uint64_t, NUM_BRACKETS> entries{
    60122219, // me, Hoops, There It Is! (Martin, martinisquared)
    62328104, // Tara, TheGambler46.  Mostly 538 with a few tweaks.
    58407997, // Dan (Dan Murphy, dmurph888).  2MW Auburn to win, 3E Purdue over 2E Kentucky for Final 4.  2S Villanova over 1S Arizona.
    58468455, // Eileen-er-iffic (Eileen (Dan's Sister), The_MRF_NYC) 3E Purdue to win.  4MW Providence over 1 MW Kansas.
    61439241, // Vakidis (Billy (Maureen's Husband), wgarbarini) 3E Purdue over 2E Kentucky. 2S Villanova over 1S Arizona.  5MW Iowa over 1MW Kansas.  2S Villanova over 1MW Kansas to make it to championship.
    62484411, // Owe'n Charlie '22 (Uncle Dennis, tiger72pu).  2S Villanova over 1S Arizona.
    65481077, // Maureen's Annual Bonus (Joe (Eileen's Husband), gettinpiggywitit) 1S Arizona to win.  Sweet 16: 3S Tennessee over 2S Villanova.  3MW Wisconsin over 2MW Auburn.
    69629125, // RPcatsmounts! espn88461517 (Ryan Price, Dan's step brother).  1MW Kansas to win. 3E Purdue over 2E Kentucky. 3S Tennessee over 1S Arizona.
              // 61783453,  # Villa-Mo-va 1 (Maureen (Dan's Sister), Villa-Mo-va)
};

/**********  Utilities: asserts and random number generator  **********/

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

time_point<high_resolution_clock> now()
{
   return high_resolution_clock::now();
}

template <typename Clock>
double elapsed(time_point<Clock> start, time_point<Clock> end)
{
   return duration_cast<microseconds>(end - start).count() / 1e6;
}

uint64_t get_urandom()
{
   int fd = open("/dev/urandom", O_RDONLY);
   if (fd < 0)
   {
      cerr << "Failed to open /dev/urandom\n";
      abort();
   }

   uint64_t result;
   ssize_t bytes_read = read(fd, &result, sizeof(uint64_t));
   if (bytes_read != sizeof(uint64_t))
   {
      cerr << "Error reading from /dev/urandom\n";
      abort();
   }

   return result;
}

// From Numerical Recipes, Third Edition, Section 7.1.3
class Rand
{
public:
   Rand(uint64_t seed) : state(4101842887655102017LL)
   {
      state ^= seed;
      state = uint64();
   }

   Rand() : Rand(get_urandom()) {}

   uint64_t uint64()
   {
      state ^= state >> 21;
      state ^= state << 35;
      state ^= state >> 4;
      return state * 2685821657736338717LL;
   }

   double uniform()
   {
      return (1.0 / static_cast<double>(numeric_limits<uint64_t>::max())) * uint64();
   }

private:
   uint64_t state;
};

Rand rng;

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

struct Team
{
   string name;
   string abbrev;
};

vector<Team> teams(NUM_TEAMS);
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
   assert(winner >= 0);
   assert(first >= 0);
   assert(second >= 0);
   assert(winner == first || winner == second);

   double result = get_prob(winner, round) / (get_prob(first, round) + get_prob(second, round));
   if (isnan(result))
   {
      cout << "first: " << (int)first << " " << teams[first].name << ", second: " << (int)second << " " << teams[second].name << ", winner: " << (int)winner << ", round: " << round << endl;
      cout << get_prob(winner, round) << endl;
      cout << get_prob(first, round) << endl;
      cout << get_prob(second, round) << endl;
   }
   assert(!isnan(result));
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
      assert(raw[raw.size() - 1] == '\'');
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
   assert(1 <= match && match <= NUM_GAMES);
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

const array<const string, NUM_ROUNDS> round_names{
    "Champ",
    "Final4",
    "Elite8",
    "Sweet16",
    "Roundof32",
    "Roundof64",
};

unordered_map<game_t, string> INDEX_TO_NAME{
    {0, "Round of 64"},
    {32, "Round of 32"},
    {32 + 16, "Sweet 16"},
    {32 + 16 + 8, "Elite 8"},
    {32 + 16 + 8 + 4, "Final 4"},
    {32 + 16 + 8 + 4 + 2, "Championship"},
};

/**********  Tuples of scores, packed in an int  **********/

// This should probably be a simple class, rather than a typedef.  Oh well.
using scoretuple_t = uint64_t; // For more than 8 players, need uint128_t.
static_assert(sizeof(scoretuple_t) >= NUM_BRACKETS);

pair<int, int> winner(scoretuple_t scores)
{
   const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&scores);

   uint8_t biggest = bytes[0];
   int biggest_index = 0;
   uint8_t second_biggest = 0;
   int second_biggest_index = -1;
   for (size_t i = 1; i < NUM_BRACKETS; i++)
   {
      if (bytes[i] > biggest)
      {
         second_biggest = biggest;
         second_biggest_index = biggest_index;
         biggest = bytes[i];
         biggest_index = i;
      }
      else if (second_biggest_index < 0 || bytes[i] > second_biggest)
      {
         second_biggest = bytes[i];
         second_biggest_index = i;
      }
   }

   return make_pair(biggest_index, second_biggest_index);
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
          teams[matchup.first_team].name + ", " + teams[matchup.second_team].name + ", " +
          (matchup.winner >= 0 ? teams[matchup.winner].name : "None");
}

Matchup parse_matchup(const json &matchup)
{
   Matchup result;
   result.id = matchup["id"].get<int>() - 63;
   const auto &match_teams = matchup["o"];
   if (match_teams.size() >= 1)
   {
      result.first_team = match_teams[0]["id"].get<int>() - 65;
      assert(teams[result.first_team].name == match_teams[0]["n"]);
   }
   if (match_teams.size() >= 2)
   {
      result.second_team = match_teams[1]["id"].get<int>() - 65;
      assert(teams[result.second_team].name == match_teams[1]["n"]);
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

// vector<Bracket> brackets;

string to_string(const Bracket &bracket)
{
   string result;
   for (game_t match = 0; match < NUM_GAMES; ++match)
   {
      auto iter = INDEX_TO_NAME.find(match);
      if (iter != INDEX_TO_NAME.end())
      {
         result += "*****  " + iter->second + "\n";
      }
      result += to_string(match) + ": " + teams[bracket.picks[match]].name + "\n";
   }
   return result;
}

Bracket make_bracket(const array<bool, NUM_GAMES> &who_wins, string name = "Experiment")
{
   Bracket bracket;
   bracket.name = name;
   for (game_t match = 0; match < NUM_GAMES; ++match)
   {
      if (match < 32)
      {
         // Base case: round of 64.
         bracket.picks.push_back(match * 2 + (who_wins[match] ? 0 : 1));
      }
      else
      {
         bracket.picks.push_back(bracket.picks[input(match) + (who_wins[match] ? 0 : 1)]);
      }
   }

   return bracket;
}

// In 2022, chance of winning whole thing was 21.53%.
pair<Bracket, array<bool, NUM_GAMES>> make_most_likely_bracket()
{
   Bracket bracket;
   bracket.name = "Most Likely";
   array<bool, NUM_GAMES> choices;

   for (game_t match = 0; match < NUM_GAMES; ++match)
   {
      int round = round_index(match + 1).round;
      team_t first_team;
      team_t second_team;
      if (match < 32)
      {
         first_team = games[match].first_team;
         second_team = games[match].second_team;
      }
      else
      {
         int prev = input(match);
         first_team = bracket.picks[prev];
         second_team = bracket.picks[prev + 1];
      }
      // So this is not actually the criteria we want to use, since it's
      // conditional on previous choices being correct.  What we want is the
      // overall probability of both getting to this round & winning it, which
      // is what 538 and Ken Pomeroy have in their raw numbers.
      //
      // Note that this actually happened in 2022 in the Sweet 16, for Duke vs
      // Texas Tech.  Texas Tech was the stronger team, but was less likely to
      // get to the Sweet 16, and thus less likely to win the Sweet 16 overall
      // than Duke.  This code chose Sweet 16, but the first single_optimize()
      // fixed it up.
      double first_prob = game_prob(first_team, second_team, first_team, round);
      choices[match] = first_prob >= 0.5;
      bracket.picks.push_back(first_prob >= 0.5 ? first_team : second_team);
   }

   return make_pair(bracket, choices);
}

void compare(const Bracket &bracket1, const Bracket &bracket2)
{
   cout << fmt::format("\n     {:<20} {}\n", bracket1.name, bracket2.name);
   cout << fmt::format("     ---------------      --------------\n");
   for (game_t match = 0; match < NUM_GAMES; ++match)
   {
      auto iter = INDEX_TO_NAME.find(match);
      if (iter != INDEX_TO_NAME.end())
      {
         cout << iter->second << "\n";
      }
      cout << fmt::format("{} {:<2} {:<20} {}\n",
                          bracket1.picks[match] == bracket2.picks[match] ? " " : "*",
                          match,
                          teams[bracket1.picks[match]].name,
                          teams[bracket2.picks[match]].name);
   }
}

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
         result += teams[i].name;
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
   assert(result.picks.size() == NUM_GAMES);

   return result;
}

void make_all_selections(const vector<Bracket> &brackets)
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

#if WITH_BOOLEXPR

class BoolExpr
{
public:
   virtual ~BoolExpr() {}

   virtual string to_string() const = 0;
   virtual string sexpr(int indent) const = 0;
};

string to_string(const shared_ptr<BoolExpr> &ptr)
{
   if (ptr)
   {
      return ptr->to_string();
   }

   return "empty";
}

struct Var : public BoolExpr
{
   const string name;
   const size_t index;
   const bool first;

   Var(string name, size_t index, bool first) : name(name), index(index), first(first) {}

   string to_string() const override
   {
      return name;
   }

   string sexpr(int /*indent*/) const override
   {
      return name;
   }

   const shared_ptr<Var> &negation() const
   {
      if (first)
      {
         return all_vars[index].second;
      }
      else
      {
         return all_vars[index].first;
      }
   }

   static vector<pair<shared_ptr<Var>, shared_ptr<Var>>>
       all_vars;
};

vector<pair<shared_ptr<Var>, shared_ptr<Var>>> Var::all_vars(NUM_TEAMS);

class InfixExpr : public BoolExpr
{
public:
   // I wonder if this should be a vector, since that would be more readable.
   const unordered_set<shared_ptr<BoolExpr>> children;

   InfixExpr(unordered_set<shared_ptr<BoolExpr>> kids) : children{std::move(kids)} {}

   virtual pair<string, string> empty_and_operation() const = 0;

   virtual string to_string() const override
   {
      auto empty_and_op = empty_and_operation();
      if (children.empty())
      {
         return empty_and_op.first;
      }

      string delim = "(";
      string result;
      for (const auto &child : children)
      {
         result += delim + ::to_string(child);
         delim = " " + empty_and_op.second + " ";
      }
      return result + ")";
   }

   virtual string sexpr(int indent) const override
   {
      if (indent == 6)
      {
         return "...";
      }
      if (indent > 6)
      {
         return "";
      }
      auto empty_and_op = empty_and_operation();
      if (children.empty())
      {
         return empty_and_op.first;
      }

      string result = "(" + empty_and_op.second + "\n";
      for (const auto &child : children)
      {
         result += string(indent, ' ') + child->sexpr(indent + 2) + "\n";
      }
      return result + string(indent, ' ') + ")";
   }
};

class OrExpr : public InfixExpr
{
public:
   OrExpr(unordered_set<shared_ptr<BoolExpr>> kids) : InfixExpr{std::move(kids)} {}

   pair<string, string> empty_and_operation() const
   {
      return make_pair(string("false"), string("or"));
   };
};

class AndExpr : public InfixExpr
{
public:
   AndExpr(unordered_set<shared_ptr<BoolExpr>> kids) : InfixExpr{std::move(kids)} {}

   pair<string, string> empty_and_operation() const
   {
      return make_pair(string("true"), string("and"));
   };
};

void remove_empty(unordered_set<shared_ptr<BoolExpr>> &children)
{
   auto iter = children.begin();
   while (iter != children.end())
   {
      auto next = iter;
      next++;
      if (iter->get() == nullptr)
      {
         children.erase(iter);
      }
      iter = next;
   }
}

struct FactorResults
{
   unordered_set<shared_ptr<BoolExpr>> reduced_first;
   unordered_set<shared_ptr<BoolExpr>> reduced_second;
   unordered_set<shared_ptr<BoolExpr>> common;
};

FactorResults factor_helper(
    const unordered_set<shared_ptr<BoolExpr>> &first_children,
    const unordered_set<shared_ptr<BoolExpr>> &second_children)
{
   FactorResults results;

   // Copy second->children into second_children.
   for (auto child2 : second_children)
   {
      results.reduced_second.insert(child2);
   }

   for (auto &child1 : first_children)
   {
      auto in_second = results.reduced_second.find(child1);
      if (in_second != results.reduced_second.end())
      {
         results.reduced_second.erase(in_second);
         results.common.insert(child1);
      }
      else
      {
         results.reduced_first.insert(child1);
      }
   }

   return results;
}

template <typename ExprT>
struct Dual;

template <>
struct Dual<AndExpr>
{
   using type = OrExpr;
};

template <>
struct Dual<OrExpr>
{
   using type = AndExpr;
};

template <typename ExprT>
unordered_set<shared_ptr<BoolExpr>> flatten(unordered_set<shared_ptr<BoolExpr>> children)
{
   unordered_set<shared_ptr<BoolExpr>> new_kids; // on the block.

   for (const auto &child : children)
   {
      if (auto target_expr = dynamic_cast<ExprT *>(child.get()))
      {
         new_kids.insert(target_expr->children.begin(), target_expr->children.end());
      }
      else
      {
         new_kids.insert(child);
      }
   }

   return new_kids;
}

template <typename OuterExprT>
pair<shared_ptr<BoolExpr>, bool> factor(const shared_ptr<BoolExpr> &first_in, const shared_ptr<BoolExpr> &second_in);

// A and (notA or B) <==> A and B
// A or (notA and B) <==> A or B
//
// In general:
// A and X1 and X2 and (notA or B1 or B2) <==> A and X1 and X2 and (B1 or B2)

// This function, also factor(), and the boolean simplifier in general, are
// complicated enough that they need unit tests.
template <typename OuterExprT>
shared_ptr<BoolExpr> special_helper(const unordered_set<shared_ptr<BoolExpr>> &source_children)
{
   using InnerExprT = typename Dual<OuterExprT>::type;

   for (const auto &source_child : source_children)
   {
      if (auto source_child_var = dynamic_cast<Var *>(source_child.get()))
      {
         const auto &negation = source_child_var->negation();
         // Now we need to find a second child, of the dual type.
         for (auto dual_iter = source_children.begin(); dual_iter != source_children.end(); ++dual_iter)
         {
            if (auto dual = dynamic_cast<InnerExprT *>(dual_iter->get()))
            {
               auto negation_iter = dual->children.find(negation);
               if (negation_iter != dual->children.end())
               {
                  // We can simplify!
                  // Now we copy all of the children except this one.
                  //
                  // At this point:
                  // source_child, source_child_var point to the "outer" var, child of source.
                  // dual_iter, dual point to the dual, child of source.
                  // negation_iter points to the negation of var, child of dual.
                  // cout << "Found " << source_child_var->to_string() << " in outer, and " << to_string(*negation_iter) << " in inner.\n";

                  bool raised_node = false;
                  unordered_set<shared_ptr<BoolExpr>> result;
                  for (auto dual_iter2 = source_children.begin(); dual_iter2 != source_children.end(); ++dual_iter2)
                  {
                     if (dual_iter2 != dual_iter)
                     {
                        result.insert(*dual_iter2);
                     }
                     else
                     {
                        // For this child, take everything but the negation var.
                        unordered_set<shared_ptr<BoolExpr>> new_dual_children;
                        for (auto dual_child_iter = dual->children.begin(); dual_child_iter != dual->children.end(); ++dual_child_iter)
                        {
                           if (dual_child_iter != negation_iter)
                           {
                              new_dual_children.insert(*dual_child_iter);
                           }
                        }
                        assert(new_dual_children.size() >= 1);
                        if (new_dual_children.size() == 1)
                        {
                           raised_node = true;
                           result.insert(*new_dual_children.begin());
                        }
                        else
                        {
                           result.insert(make_shared<InnerExprT>(std::move(new_dual_children)));
                        }
                     }
                  }
                  // cout << "Replacing " << to_string(make_shared<OuterExprT>(source_children)) << " with " << to_string(make_shared<OuterExprT>(result)) << "\n";
                  if (raised_node)
                  {
                     return make_helper<OuterExprT>(result);
                  }
                  return make_helper<OuterExprT>(result);
               }
            }
         }
      }
   }

   return make_shared<OuterExprT>(source_children);
}

shared_ptr<BoolExpr> special(const shared_ptr<BoolExpr> source)
{
   if (auto and_expr = dynamic_cast<const AndExpr *>(source.get()))
   {
      return special_helper<AndExpr>(and_expr->children);
   }
   else if (auto or_expr = dynamic_cast<const OrExpr *>(source.get()))
   {
      return special_helper<OrExpr>(or_expr->children);
   }
   return source;
}

template <typename ExprT>
shared_ptr<BoolExpr> make_helper(unordered_set<shared_ptr<BoolExpr>> children, bool try_factor = false)
{
   if (children.size() == 1)
   {
      return *children.begin();
   }

   children = flatten<ExprT>(children);

   if (children.size() == 1)
   {
      return *children.begin();
   }

   for (const auto &child : children)
   {
      if (auto child_var = dynamic_cast<Var *>(child.get()))
      {
         if (children.find(child_var->negation()) != children.end())
         {
            return make_shared<typename Dual<ExprT>::type>(unordered_set<shared_ptr<BoolExpr>>{});
         }
      }
   }

   if (try_factor)
   {
      vector<shared_ptr<BoolExpr>> kids;
      for (const auto &child : children)
      {
         kids.emplace_back(child);
      }
      bool modified = false;

      children.clear();
      while (kids.size() >= 2)
      {
         bool got_one = false;
         for (size_t j = 0; j < kids.size() - 1; ++j)
         {
            auto [expr, factored] = factor<ExprT>(kids[j], kids[kids.size() - 1]);
            if (factored)
            {
               modified = true;
               got_one = true;
               children.insert(expr);
               kids.erase(kids.begin() + j);
               break;
            }
         }
         if (!got_one)
         {
            children.insert(kids[kids.size() - 1]);
         }
         kids.pop_back();
      }
      for (auto &kid : kids)
      {
         children.insert(kid);
      }

      // Only recursively call ourselves if we actually factored something out
      // above, to avoid infinite recursion.
      if (modified)
      {
         // return make_helper<ExprT>(std::move(children));
         return special(make_helper<ExprT>(std::move(children)));
      }
   }

   // return make_shared<ExprT>(std::move(children));
   return special(make_shared<ExprT>(std::move(children)));
}

// (A & B) | (A & C) == A & (B | C)
// (A | B) & (A | C) == A | (B & C)
// Generalized to:
// (A inner B) outer (A inner C) => A inner (B outer C)
//
// (A1 inner A2 inner B1 inner B2) outer (A1 inner A2 inner C1 inner C2) =>
//     A1 inner A2 inner ((B1 inner B2) outer (C1 inner C2))

// Bool in return: "false" means we didn't factor anything out, and return
// expression is just the two inputs.  "true" means we did factor something out.
template <typename OuterExprT>
pair<shared_ptr<BoolExpr>, bool> factor(const shared_ptr<BoolExpr> &first_in, const shared_ptr<BoolExpr> &second_in)
{
   using InnerExprT = typename Dual<OuterExprT>::type;

   const InnerExprT *first = dynamic_cast<const InnerExprT *>(first_in.get());
   const InnerExprT *second = dynamic_cast<const InnerExprT *>(second_in.get());

   if (!first || !second)
   {
      return make_pair(make_shared<OuterExprT>(unordered_set<shared_ptr<BoolExpr>>{first_in, second_in}), false);
   }

   FactorResults factored = factor_helper(first->children, second->children);

   if (factored.common.empty())
   {
      return make_pair(make_helper<OuterExprT>(unordered_set<shared_ptr<BoolExpr>>{first_in, second_in}), false);
   }

   if (factored.reduced_first.empty() || factored.reduced_second.empty())
   {
      return make_pair(make_helper<InnerExprT>(std::move(factored.common)), true);
   }

   auto temp =
       make_helper<OuterExprT>({make_helper<InnerExprT>(std::move(factored.reduced_first)),
                                make_helper<InnerExprT>(std::move(factored.reduced_second))},
                               true /* try_factor */);

   factored.common.insert(std::move(temp));
   return make_pair(make_helper<InnerExprT>(std::move(factored.common)), true);
}

shared_ptr<BoolExpr> or_(unordered_set<shared_ptr<BoolExpr>> children)
{
   remove_empty(children);

   return make_helper<OrExpr>(children, true);
}

shared_ptr<BoolExpr> and_(unordered_set<shared_ptr<BoolExpr>> children)
{
   remove_empty(children);

   return make_helper<AndExpr>(children, true);

   // return make_shared<AndExpr>(flatten<AndExpr>(children));
}

#endif // WITH_BOOLEXPR

/**********  Outcomes  **********/

scoretuple_t
get_scoretuple(game_t match_index, team_t winning_team, uint8_t reduced_points, const vector<Bracket> &brackets)
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

struct ResultSet
{
   double prob;
#if WITH_BOOLEXPR
   shared_ptr<BoolExpr> which;
#endif

   void combine_disjoint(const ResultSet other)
   {
      prob += other.prob;
#if WITH_BOOLEXPR
      which = or_({which, other.which});
#endif
   }
};

string to_string(ResultSet result_set)
{
   return fmt::format("{:.3f}% ", result_set.prob * 100) BOOLEXPR(+to_string(result_set.which));
}

struct TeamInfo
{
   team_t team;
   ResultSet result_set;
};

struct Row
{
   scoretuple_t scoretuple;
   ResultSet result_set;
   double cumsum_prob = 0;
};

struct Outcomes
{
   team_t team;
   unordered_map<scoretuple_t, ResultSet> result_sets;

private:
   // total_prob_ is only used during Monte Carlo.
   double total_prob_ = 0;
   mutable bool frozen_ = false;
   mutable vector<Row> rows_;

public:
   Outcomes(team_t team, scoretuple_t scores, ResultSet set) : team(team), result_sets{{scores, set}}, total_prob_(set.prob) {}
   Outcomes() : team(-1) {}
   Outcomes(team_t team) : team(team) {}

   double total_prob() const
   {
      return total_prob_;
   }

   void increment_total_prob(double amount)
   {
      assert(!frozen_);
      if (amount + total_prob_ > 1.0000001)
      {
         cout << "total_prob_: " << total_prob_ << ", increment: " << amount << endl;
      }
      assert(amount + total_prob_ < 1.0000001);
      total_prob_ += amount;
   }

   const vector<Row> &get_rows() const
   {
      if (!frozen_)
      {
         double cumsum_prob = 0;
         for (const auto &[scoretuple, result_set] : result_sets)
         {
            cumsum_prob += result_set.prob;
            rows_.push_back({scoretuple, result_set, cumsum_prob});
            rows_[rows_.size() - 1].result_set.prob = -1;
         }
         if (fabs(total_prob() - rows_[rows_.size() - 1].cumsum_prob) > 1e-12)
         {
            cout << "total_prob: " << total_prob() << ", computed: " << rows_[rows_.size() - 1].cumsum_prob << ", diff: " << total_prob() - rows_[rows_.size() - 1].cumsum_prob << "\n";
         }
         assert(fabs(total_prob() - rows_[rows_.size() - 1].cumsum_prob) < 1e-12);
         frozen_ = true;
      }
      return rows_;
   }

   void update(const TeamInfo &winner, scoretuple_t this_scores, scoretuple_t total_scores, const ResultSet &result_set1, const ResultSet &result_set2,
               double probability)
   {
      assert(!frozen_);
      ResultSet new_set;

      if (winner.team < 0)
      {
#if WITH_BOOLEXPR
         new_set.which = and_({result_set1.which, result_set2.which});
#endif
      }
      else
      {
#if WITH_BOOLEXPR
         new_set.which = and_({result_set1.which, result_set2.which, winner.result_set.which});
#endif
         total_scores += this_scores;
      }
      // This is where I do the "or" with existing results.;
      ResultSet &rset = result_sets[normalize(total_scores)];
      new_set.prob = winner.result_set.prob * probability;
      rset.combine_disjoint(new_set);
   }
};

string to_string(const Outcomes &outcome)
{
   string result;
   result += (outcome.team < 0 ? "other" : teams[outcome.team].name) + ":\n";
   for (const auto &scores : outcome.result_sets)
   {
      result += "    " + make_string(scores.first) + " " + to_string(scores.second) + "\n";
   }
   return result;
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

const Row &random_row(const vector<Row> &rows)
{
   double myrand = rng.uniform() * rows[rows.size() - 1].cumsum_prob;

   // First item where myrand <= item.cumsum_prob
   auto iter = lower_bound(rows.begin(), rows.end(), myrand, [](const Row &row, double myrand)
                           { return row.cumsum_prob < myrand; });
   assert(iter != rows.end());

   return *iter;
}

// The first element of the vector is always for team "other".
vector<Outcomes>
outcomes(
    game_t match_index,
    bitset<64> selections,
    const vector<Bracket> &brackets)
{
   const Matchup &game = games[match_index];
   const auto ri = round_index(match_index + 1);
   int this_points = points_per_match[match_index];
   if (ri.round == NUM_ROUNDS - 1)
   {
      // Base case, round of 64.
      assert(this_points == 10);
      vector<Outcomes> result(1);
      vector<TeamInfo> teams_with_probs;
      if (game.winner >= 0)
      {
         teams_with_probs.push_back({game.winner, {1.0 BOOLEXPR(COMMA{})}});
      }
      else
      {
         assert(game.first_team >= 0);
         assert(game.second_team >= 0);
         double prob_first = game_prob(game.first_team, game.second_team, game.first_team, ri.round);
         teams_with_probs.push_back({game.first_team, {prob_first BOOLEXPR(COMMA Var::all_vars[game.id].first)}});
         teams_with_probs.push_back({game.second_team, {1.0 - prob_first BOOLEXPR(COMMA Var::all_vars[game.id].second)}});
      }

      for (const TeamInfo &team_with_prob : teams_with_probs)
      {
         auto scores = get_scoretuple(match_index, team_with_prob.team, this_points / 10, brackets);
         assert(team_with_prob.team >= 0);

         if (true /* selections[team_with_prob.team] */)
         {
            result.emplace_back(team_with_prob.team, scores, team_with_prob.result_set);
         }
         else
         {
            result[0].result_sets[scores].prob += team_with_prob.result_set.prob;
         }
      }

      return result;
   }

   // General case.  Start by recursing.
   int prev_match = input(match_index);
   const auto outcomes1 = outcomes(prev_match, all_selections[match_index], brackets);
   const auto outcomes2 = outcomes(prev_match + 1, all_selections[match_index], brackets);

   size_t threshold_per_team_pairs = MONTE_CARLO_THRESHOLD / (double)(outcomes1.size() * outcomes2.size());

   // auto start = now();

   vector<Outcomes> result(1);

   /*
   if (ri.round <= 1)
   {
      cout << "About to loop, round " << ri.round << endl;
   }
   */

   bool displayed_mc_warning = false;
   // double rows_elapsed = 0;
   // double mc_iters_elapsed = 0;

   for (const Outcomes &outcome1 : outcomes1)
   {
      if (outcome1.result_sets.empty())
      {
         continue;
      }
      assert(outcome1.team >= 0);

      for (const Outcomes &outcome2 : outcomes2)
      {
         if (outcome2.result_sets.empty())
         {
            continue;
         }
         assert(outcome2.team >= 0);

         vector<TeamInfo> teams_with_probs;
         if (game.winner >= 0)
         {
            // If this game has been played in real life, then all previous games
            // have also been played, so our recursive outcomes had better have
            // only a single non-empty result.
            assert(outcomes1.size() == 1 || (outcomes1.size() == 2 && outcomes1[0].result_sets.empty()));
            assert(outcomes2.size() == 1 || (outcomes2.size() == 2 && outcomes2[0].result_sets.empty()));
            assert(game.winner == outcome1.team || game.winner == outcome2.team);
            teams_with_probs.push_back({game.winner, {1.0 BOOLEXPR(COMMA{})}});
         }
         else
         {
            double prob_first = game_prob(outcome1.team, outcome2.team, outcome1.team, ri.round);
            teams_with_probs.push_back({outcome1.team, {prob_first BOOLEXPR(COMMA Var::all_vars[match_index].first)}});
            teams_with_probs.push_back({outcome2.team, {1.0 - prob_first BOOLEXPR(COMMA Var::all_vars[match_index].second)}});
         }

         // So if both are "other", do we even need to loop over two winners?
         // Since the scores will be exactly the same either way?
         for (const auto &winner : teams_with_probs)
         {
            assert(winner.team >= 0);
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

            dest->increment_total_prob(winner.result_set.prob * outcome1.total_prob() * outcome2.total_prob());

            auto this_scores = get_scoretuple(match_index, winner.team, this_points / 10, brackets);

            if (outcome1.result_sets.size() * outcome2.result_sets.size() > threshold_per_team_pairs)
            {
               if (!displayed_mc_warning)
               {
                  cout << "Warning: Round " << round_names[ri.round] << " using Monte Carlo simulation, results approximate.\n";
                  displayed_mc_warning = true;
               }
               double team_pair_prob = outcome1.total_prob() * outcome2.total_prob();
               size_t monte_carlo_iters = max((size_t)(team_pair_prob * MONTE_CARLO_ITERS + 0.5), (size_t)1);

               // auto rows_start = now();
               const vector<Row> &rows1 = outcome1.get_rows();
               const vector<Row> &rows2 = outcome2.get_rows();
               // rows_elapsed += elapsed(rows_start, now());

               // auto mc_iters_start = now();
               for (size_t i = 0; i < monte_carlo_iters; ++i)
               {
                  const Row &rand_row1 = random_row(rows1);
                  const Row &rand_row2 = random_row(rows2);
                  dest->update(winner, this_scores, rand_row1.scoretuple + rand_row2.scoretuple,
                               rand_row1.result_set, rand_row2.result_set,
                               team_pair_prob / monte_carlo_iters);
               }
               // mc_iters_elapsed += elapsed(mc_iters_start, now());
            }
            else
            {
               for (const auto &[scoretuple1, result_set1] : outcome1.result_sets)
               {
                  for (const auto &[scoretuple2, result_set2] : outcome2.result_sets)
                  {
                     dest->update(winner, this_scores, scoretuple1 + scoretuple2, result_set1, result_set2,
                                  result_set1.prob * result_set2.prob);
                  }
               }
            }
         }
      }
   }

   /*
   if (ri.round <= 1)
   {
      cout << "Elapsed " << elapsed(start, now()) << " sec.\n";
      if (rows_elapsed > 0)
      {
         cout << "get_rows: " << rows_elapsed << " sec, Monte Carlo iters: " << mc_iters_elapsed << " sec\n";
      }
   }
   */

   return result;
}

struct WinProb
{
   int bracket;
   ResultSet first_place;
   ResultSet second_place;
};

vector<WinProb>
get_win_probs(const vector<Outcomes> &outcomes)
{
   vector<WinProb> win_probs(NUM_BRACKETS);

   for (size_t i = 0; i < NUM_BRACKETS; ++i)
   {
      win_probs[i].bracket = i;
   }

   for (const Outcomes &outc : outcomes)
   {
      for (const auto &score_and_result_sets : outc.result_sets)
      {
         auto [biggest_index, second_biggest_index] = winner(score_and_result_sets.first);
         win_probs[biggest_index].first_place.combine_disjoint(score_and_result_sets.second);
         win_probs[second_biggest_index].second_place.combine_disjoint(score_and_result_sets.second);
      }
   }

   return win_probs;
}

team_t must_win(game_t match_index, int bracket, const vector<Bracket> &brackets)
{
   Matchup &matchup = games[match_index];
   auto original_winner = matchup.winner;

   matchup.winner = matchup.first_team;
   assert(matchup.winner >= 0);
   auto win_probs = get_win_probs(outcomes(62, {}, brackets));
   if (win_probs[bracket].first_place.prob == 0)
   {
      matchup.winner = original_winner;
      return matchup.second_team;
   }

   matchup.winner = matchup.second_team;
   assert(matchup.winner >= 0);
   win_probs = get_win_probs(outcomes(62, {}, brackets));
   if (win_probs[bracket].first_place.prob == 0)
   {
      matchup.winner = original_winner;
      return matchup.first_team;
   }

   matchup.winner = original_winner;
   return -1;
}

void alternatives(int bracket_to_consider, const vector<team_t> &matches_to_consider, const vector<Bracket> &brackets)
{
   vector<game_t> single_eliminated;
   for (game_t match_index : matches_to_consider)
   {
      team_t team_must_win = must_win(match_index, bracket_to_consider, brackets);

      if (team_must_win > 0)
      {
         cout << "::::: " << teams[team_must_win].name << " (game " << (int)match_index << ") must win or bracket will be eliminted!\n";
         single_eliminated.push_back(match_index);
      }
   }
   for (size_t i = 0; i < matches_to_consider.size(); i++)
   {
      team_t first_match_index = matches_to_consider[i];
      if (find(single_eliminated.begin(), single_eliminated.end(), first_match_index) != single_eliminated.end())
      {
         continue;
      }

      Matchup &matchup = games[first_match_index];
      auto original_winner = matchup.winner;

      matchup.winner = matchup.first_team;
      assert(matchup.winner >= 0);
      for (size_t j = i + 1; j < matches_to_consider.size(); j++)
      {
         team_t second_match_index = matches_to_consider[j];
         if (find(single_eliminated.begin(), single_eliminated.end(), second_match_index) != single_eliminated.end())
         {
            continue;
         }
         team_t team_must_win = must_win(second_match_index, bracket_to_consider, brackets);

         if (team_must_win > 0)
         {
            cout << "***** Either " << teams[matchup.second_team].name << " (game " << (int)first_match_index << ") or " << teams[team_must_win].name << " (game " << (int)second_match_index << ") must win or bracket will be eliminted!\n";
         }
      }

      matchup.winner = matchup.second_team;
      assert(matchup.winner >= 0);
      for (size_t j = i + 1; j < matches_to_consider.size(); j++)
      {
         team_t second_match_index = matches_to_consider[j];
         if (find(single_eliminated.begin(), single_eliminated.end(), second_match_index) != single_eliminated.end())
         {
            continue;
         }
         team_t team_must_win = must_win(second_match_index, bracket_to_consider, brackets);

         if (team_must_win > 0)
         {
            cout << "***** Either " << teams[matchup.first_team].name << " (game " << (int)first_match_index << ") or " << teams[team_must_win].name << " (game " << (int)second_match_index << ") must win or bracket will be eliminted!\n";
         }
      }
      matchup.winner = original_winner;
   }
}

/**********  Probablity of winning  **********/

// 25.09% chance of success.
array<bool, NUM_GAMES> best_choices_2022{true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, false, false, true, false, false, false, true, true, false, false, true, true, false, false, true, true, true, false, true, false, false, false, true, false, false, true, true, true, true};

/*
array<bool, NUM_GAMES> best_choices_2022{true, false, true, true, true, true, true,
                                         true, true, true, true, true, true, true, true,
                                         true, true, false, true, true, false, true,
                                         true, true, true, true, true, true, true,
                                         true, true, true, true, true, false, false,
                                         true, false, false, false, true, true, false,
                                         false, true, true, false, false, true, false,
                                         false, false, true, true, false, false, true,
                                         false, true, true, true, true, true};
*/

string to_string(const array<bool, NUM_GAMES> &choices)
{
   bool first = true;
   string result{"{"};
   for (bool choice : choices)
   {
      if (!first)
      {
         result += ", ";
      }
      result += (choice ? "true" : "false");
      first = false;
   }
   return result + "}";
}

double prob_win(const array<bool, NUM_GAMES> &choices, int entry, const vector<Bracket> &orig_brackets)
{
   vector<Bracket> brackets{orig_brackets};

   brackets[entry] = make_bracket(choices);
   make_all_selections(brackets);

   auto results = outcomes(NUM_GAMES - 1, {}, brackets);

   auto win_probs = get_win_probs(results);

   return win_probs[entry].first_place.prob;
}

struct Stuff
{
   array<bool, NUM_GAMES> choices;
   string description;
   double prob = -1;
};

class OptimGenerator
{
public:
   virtual ~OptimGenerator() {}
   virtual Stuff operator()() = 0;
};

class Distributor
{
public:
   Distributor(unique_ptr<OptimGenerator> generator) : generator_(std::move(generator)) {}

private:
   unique_ptr<OptimGenerator> generator_;
};

// Could parallelize the optimize stuff:
// - Have a class that generates the next "choices" array.  Use a mutex to
//   protect.  Simple.  Worker threads query it in a loop, compute the
//   probabilitiy, then put that probability, along with the array, on a
//   "completed work" queue, and release() the counting_semaphore<>.
// - The main thread pulls from the "completed work" queue, using a
//   counting_semaphore<> acquire().  It can then decide whether we have a new
//   "best", and if so, print out whatever it wants, if not, just print a
//   status.  Basically, the main thread handles all the printing, so we don't
//   get mixed output lines.

pair<array<bool, NUM_GAMES>, double> single_optimize(array<bool, NUM_GAMES> best_choices, double best_prob, int entry_to_optimize, game_t first_match, const vector<Bracket> &brackets)
{
   for (game_t match = first_match; match < NUM_GAMES; ++match)
   {
      array<bool, NUM_GAMES> this_choices = best_choices;
      this_choices[match] = !this_choices[match];

      double prob = prob_win(this_choices, entry_to_optimize, brackets);
      cout << "prob after flipping match " << (int)match << " is " << prob * 100 << "%\n";
      if (prob > best_prob)
      {
         cout << "*****  New best!\n";
         best_choices = this_choices;
         best_prob = prob;
      }
   }

   return make_pair(best_choices, best_prob);
}

// In 2022, double_optimize() gave a benefit over single_optimize(): matches 53
// (Villanova winning Sweet 16) & 58 (Villanova winning Elite 8)
pair<array<bool, NUM_GAMES>, double> double_optimize(array<bool, NUM_GAMES> best_choices, double best_prob, int entry_to_optimize, const vector<Bracket> &brackets)
{
   for (game_t outer_match = 0; outer_match < NUM_GAMES; ++outer_match)
   {
      array<bool, NUM_GAMES> outer_choices = best_choices;
      outer_choices[outer_match] = !outer_choices[outer_match];
      double outer_prob = prob_win(outer_choices, entry_to_optimize, brackets);
      cout << "##### Outer.  Best prob so far " << best_prob * 100 << "%, flipping match " << (int)outer_match << " gives probability " << outer_prob * 100 << "%\n";
      auto [inner_best_choices, inner_best_prob] = single_optimize(outer_choices, outer_prob, entry_to_optimize, max(outer_match + 1, 32), brackets);
      if (inner_best_prob > best_prob)
      {
         best_choices = inner_best_choices;
         best_prob = inner_best_prob;
         cout << "**********  NEW OVERALL BEST!!  New best prob: " << best_prob * 100 << "%\n";
      }
   }
   return make_pair(best_choices, best_prob);
}

pair<array<bool, NUM_GAMES>, double> all_optimize(array<bool, NUM_GAMES> initial_choices, int entry_to_optimize, const vector<Bracket> &brackets)
{
   array<bool, NUM_GAMES> flipped;
   for (size_t i = 0; i < NUM_GAMES; ++i)
   {
      flipped[i] = false;
   }

   double best_prob = -1;
   int last_flipped = -1;
   int last_ever_flipped = NUM_GAMES;

   array<bool, NUM_GAMES> best_choices;
   for (;;)
   {
      array<bool, NUM_GAMES> this_choices;
      for (size_t i = 0; i < NUM_GAMES; ++i)
      {
         this_choices[i] = flipped[i] ? !initial_choices[i] : initial_choices[i];
      }

      double prob = prob_win(this_choices, entry_to_optimize, brackets);
      for (int i = last_ever_flipped; i < NUM_GAMES; ++i)
      {
         cout << (flipped[i] ? 'F' : 'S');
      }
      cout << " last ever flipped " << last_ever_flipped << ", last flipped " << last_flipped << ", prob: " << prob * 100 << "%\n";

      if (prob > best_prob)
      {
         cout << "*****  New best!\n";
         best_choices = this_choices;
         best_prob = prob;

         compare(make_bracket(best_choices, to_string(prob * 100) + "%"), make_bracket(initial_choices, "Initial"));

         cout << "array<bool, NUM_GAMES> who_wins ";
         cout << to_string(best_choices) << ";\n";
      }

      // Update flipped.
      for (int i = NUM_GAMES - 1; i >= 0; i--)
      {
         last_flipped = i;
         flipped[i] = !flipped[i];
         if (flipped[i])
         {
            break;
         }
      }

      if (last_flipped < last_ever_flipped)
      {
         last_ever_flipped = last_flipped;
      }
   }

   return make_pair(best_choices, best_prob);
}

/**********  Putting it all together  **********/

int main(int argc, char *argv[])
{
   string html = get_entry(entries[0]);
   const auto teams_json = get_json("espn.fantasy.maxpart.config.scoreboard_teams", html);
   assert(teams_json.size() == NUM_TEAMS);
   for (const auto &team : teams_json)
   {
      int id = team["id"].get<int>() - 1;
      teams[id].name = team["n"];
      teams[id].abbrev = team["a"];
      eid_to_team[team["eid"].get<int>()] = id;
   }

   const auto matchups_json = get_json("espn.fantasy.maxpart.config.scoreboard_matchups", html);

   assert(matchups_json.size() == NUM_GAMES);
   for (const auto &matchup : matchups_json)
   {
      Matchup m = parse_matchup(matchup);
      games[m.id] = m;
#if WITH_BOOLEXPR
      if (m.winner < 0)
      {
         const string &rname = round_names[round_index(m.id + 1).round];
         const string first_name = (m.first_team < 0 ? to_string(m.id) + "first" : teams[m.first_team].abbrev);
         const string second_name = (m.second_team < 0 ? to_string(m.id) + "second" : teams[m.second_team].abbrev);
         Var::all_vars[m.id] = make_pair(make_shared<Var>(first_name + "-" + rname, m.id, true),
                                         make_shared<Var>(second_name + "-" + rname, m.id, false));
      }
#endif
   }

   vector<Bracket> brackets;

   for (auto entry : entries)
   {
      brackets.push_back(get_bracket(entry));
   }

   // brackets[0] = make_bracket(best_choices_2022);

   assert(brackets.size() == NUM_BRACKETS);

   make_all_selections(brackets);

   assert(all_selections.size() == NUM_GAMES);

   parse_probs();

   /*
   for (team_t i = 48; i < 48 + 8; i++)
   {
      cout << "Game " << (int)i << ": " << teams[games[i].first_team].name << " vs " << teams[games[i].second_team].name << "\n";
   }
   cout << "Game 58 input: " << input(58) << "\n";
   cout << "   " << teams[games[input(58)].first_team].name << ", " << teams[games[input(58)].second_team].name << "\n";
   cout << "Game 61 input: " << input(61) << "\n";
   */

   compare(brackets[6], make_most_likely_bracket().first);
   compare(brackets[7], make_most_likely_bracket().first);

#if 0
   {
      cout << "**********  Top Round of 32 game in the South\n";
      // matches: 16, 17, 40
      auto result = outcomes(40, all_selections[52]);
      for (const auto &outcome : result)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   {
      cout << "**********  2nd from the top Round of 32 game in the South\n";
      // matches: 18, 19, 41
      auto result = outcomes(41, all_selections[52]);
      for (const auto &outcome : result)
      {
         if (!outcome.result_sets.empty())
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
      // matches: 16, 17, 18, 19, 40, 41, 52
      auto result = outcomes(52, all_selections[52]);
      for (const auto &outcome : result)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   // Elite 8 games:
   // 56: West
   // 57: East
   // 58: South
   // 59: Midwest

   {
      cout << "**********  Elite 8 in the South\n";
      // matches: 16, 17, 18, 19, 20, 21, 22, 23, 40, 41, 42, 43, 52, 53, 58
      auto result = outcomes(58, all_selections[58]);
      for (const auto &outcome : result)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   {
      cout << "**********  Elite 8 in the West\n";
      auto result = outcomes(56, all_selections[56]);
      for (const auto &outcome : result)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   {
      cout << "**********  Final 4 West & East\n";
      auto west_east = outcomes(60, all_selections[62]);
      for (const auto &outcome : west_east)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);
         }
      }
   }

   {
      cout << "**********  Midwest & South\n";
      auto midwest_south = outcomes(61, all_selections[62]);
      for (const auto &outcome : midwest_south)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);

            // cout << (outcome.team < 0 ? "other" : teams[outcome.team].name) << ": " << outcome.result_sets.size() << "\n";
         }
      }
   }
#endif

   vector<bool> bracket_eliminated(NUM_BRACKETS);
   {
      cout << "**********  Whole Thing!\n";
      auto start = now();
      auto results = outcomes(62, {}, brackets);
      cout << "Whole thing elapsed " << elapsed(start, now()) << " sec.\n";

      auto win_probs = get_win_probs(results);

      sort(win_probs.begin(), win_probs.end(), [](auto &a, auto &b)
           { return a.first_place.prob != b.first_place.prob ? a.first_place.prob > b.first_place.prob : a.second_place.prob != b.second_place.prob ? a.second_place.prob > b.second_place.prob
                                                                                                                                                    : a.bracket < b.bracket; });

      cout << "***** Probability of Win & 2nd place for each Bracket *****\n";
      for (int i = 0; i < NUM_BRACKETS; ++i)
      {
         int bracket_num = win_probs[i].bracket;
         cout << fmt::format("{:<22}: {:5.2f}% {:5.2f}%", brackets[bracket_num].name, win_probs[i].first_place.prob * 100, win_probs[i].second_place.prob * 100)
#if WITH_BOOLEXPR
              << (win_probs[i].first_place.which)->sexpr(0)
#endif
              << "\n";

         bracket_eliminated[bracket_num] = win_probs[i].first_place.prob == 0;
      }
   }

   const int entry_to_optimize = 0;
   // Even though Kansas (Midwest) has a higher chance to win the title than
   // Arizona (South), the optimizer says to pick Arizona.  Interesting.  In
   // fact, it says to pick Iowa (5th seed) to beat Arizona (1st seed) in Sweet
   // 16.
   // No single flips, even at 50M Monte Carlo iterations.

   /*
   // This is the best after testing all single flip optimizations.  It has
   // Arizona going to championship, against Gonzaga.  24.24% chance of winning.
   array<bool, NUM_GAMES> single_choices{true, false, true, true, true, true, true,
                                         true, true, true, true, true, true, true, true,
                                         true, true, false, true, true, false, true,
                                         true, true, true, true, true, true, true,
                                         true, true, true, true, true, false, false,
                                         true, false, false, false, true, true, false,
                                         false, true, true, false, false, true, false,
                                         false, false, true, true, false, false, true,
                                         false, true, true, true, true, true};

   // This is the best after testing all double flips.  It chooses Villanova
   // over Tennessee in the Sweet 16, and Villanova over Arizona in Elite 8.
   // Villanova then goes against Gonzaga in the championship.  24.88%
   array<bool, NUM_GAMES> double_choices{true, false, true, true, true, true, true,
                                         true, true, true, true, true, true, true, true,
                                         true, true, false, true, true, false, true,
                                         true, true, true, true, true, true, true,
                                         true, true, true, true, true, false, false,
                                         true, false, false, false, true, true, false,
                                         false, true, true, false, false, true, false,
                                         false, false, true, false, false, false, true, // <- change in this line
                                         false, false, true, true, true, true};         // <- change in this line.
   */

   // 25.09%.  So only helps over choosing the most likely (21.53%) once every 28 years.
   // No single or double flip will improve it, i.e. single_optimize() and double_optimze() don't change it.
   array<bool, NUM_GAMES> even_better{true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, false, false, true, false, false, false, true, true, false, false, true, true, false, false, true, true, true, false, true, false, false, false, true, false, false, true, true, true, true};

   /*
   array<bool, NUM_GAMES> single_from_most_likely{true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, false, false, true, false, false, false, true, true, false, false, true, true, false, false, true, false, false, false, true, true, true, false, true, false, true, true, true, false, true};
   // compare(make_bracket(single_choices, "Single Optimized"), make_bracket(double_choices, "Double Optimized"));
   // compare(make_bracket(single_choices, "Single Optimized"), make_most_likely_bracket());
   // compare(make_bracket(double_choices, "Double Optimized"), make_most_likely_bracket());
   */

   /* Found during brute force optimizing, flipping round 54 (2nd last Sweet 16).
    * 24.99%
    */
   // array<bool, NUM_GAMES> brute_force{true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true, false, true, true, true, true, true, true, true, true, true, true, true, true, true, false, false, true, false, false, false, true, true, false, false, true, true, false, false, true, true, false, false, true, false, false, false, true, false, false, true, true, true, true};

   compare(make_bracket(even_better, "Best so far"), make_most_likely_bracket().first);

   cout << "**********  Optimizer!  **********\n";
   array<bool, NUM_GAMES> to_optimize = make_most_likely_bracket().second;
   cout << to_string(make_bracket(to_optimize));

   double best_p = prob_win(to_optimize, entry_to_optimize, brackets);
   cout << "+++++ Baseline probability: " << best_p * 100 << "% +++++\n";
   // auto [best_choices, best_prob] = single_optimize(to_optimize, best_p, entry_to_optimize, 48);
   // auto [best_choices, best_prob] = double_optimize(to_optimize, best_p, entry_to_optimize);
   auto [best_choices, best_prob] = all_optimize(to_optimize, entry_to_optimize, brackets);

   cout << to_string(make_bracket(best_choices));
   cout << "array<bool, NUM_GAMES> who_wins ";
   cout << to_string(best_choices) << ";\n";
   return 0;

   struct AlternateWinProb
   {
      game_t match;
      bool first;
      team_t winning_team;
      double prob;
   };

   // TODO: automatically set matches_to_consider to all unplayed games in the
   // first round that has unplayed games.
   vector<game_t> matches_to_consider{60, 61};
   // vector<game_t> matches_to_consider{48, 49, 50, 51, 52, 53, 54, 55};

   int bracket_to_consider = 5;
   cout << "**********  " << brackets[bracket_to_consider].name << "  **********\n";
   vector<AlternateWinProb> alternate_win_probs;
   for (game_t match_index : matches_to_consider)
   {
      Matchup &matchup = games[match_index];
      auto original_winner = matchup.winner;

      matchup.winner = matchup.first_team;
      auto results = outcomes(62, {}, brackets);
      auto win_probs = get_win_probs(results);
      alternate_win_probs.push_back({match_index, true, matchup.winner, win_probs[bracket_to_consider].first_place.prob});

      matchup.winner = matchup.second_team;
      results = outcomes(62, {}, brackets);
      win_probs = get_win_probs(results);
      alternate_win_probs.push_back({match_index, false, matchup.winner, win_probs[bracket_to_consider].first_place.prob});

      matchup.winner = original_winner;
   }
   sort(alternate_win_probs.begin(), alternate_win_probs.end(), [](auto &a, auto &b)
        { return a.prob > b.prob; });

   for (const auto alt : alternate_win_probs)
   {
      cout << fmt::format("{:5.2f}% {} ({} {})\n", alt.prob * 100, teams[alt.winning_team].name, alt.match, alt.first ? "first" : "second");
   }

   for (int bracket_to_consider = 0; bracket_to_consider < NUM_BRACKETS; ++bracket_to_consider)
   {
      if (bracket_eliminated[bracket_to_consider])
      {
         continue;
      }
      cout << "**********  " << brackets[bracket_to_consider].name << "  **********\n";
      alternatives(bracket_to_consider, matches_to_consider, brackets);
   }

   return 0;
}
