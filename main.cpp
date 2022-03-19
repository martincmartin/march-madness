// To compile and run:
//
// brew install folly curl nlohmann-json fmt
// Use "brew --prefix <package>" to find where they're installed.  For me:
//
// clang++ -std=gnu++20 -O3 -DNDEBUG -Wall -Werror
// -I/opt/homebrew/opt/folly/include -L/opt/homebrew/opt/folly/lib
// -I/opt/homebrew/opt/fmt/include -L/opt/homebrew/opt/fmt/lib
// -I/opt/homebrew/opt/curl/include -L/opt/homebrew/opt/curl/lib
// -I/opt/homebrew/opt/nlohmann-json/include -lcurl -lfmt main.cpp && time
// ./a.out
#include <cstdlib>
#include <vector>
#include <iostream>
#include <sstream>
#include <regex>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <bitset>
#include <unordered_set>
#include <folly/lang/Bits.h>

constexpr size_t NUM_TEAMS = 64;
constexpr size_t NUM_GAMES = NUM_TEAMS - 1;

using game_t = int_fast8_t;
using team_t = int_fast8_t;

#define YEAR "2022"

using namespace std;

using namespace std::chrono;
using json = nlohmann::json;

array<uint64_t, 8> entries{
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

#include <curl/curl.h>

// Uses all_selections.

constexpr size_t NUM_OTHER1 = 34588;
constexpr size_t NUM_OTHER2 = 1109582;

constexpr size_t NUM_BRACKETS = 8;

int randint(int max)
{
   return random() % max;
}

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

std::string URL_FORMAT = "https://fantasy.espn.com/tournament-challenge-bracket/" YEAR
                         "/en/entry?entryID={}";

string get_entry(uint64_t entry)
{
   return get_url(fmt::format(URL_FORMAT, entry));
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
   // For x > 0, findLastSet(x) == 1 + floor(log2(x)).
   int round = folly::findLastSet(64 - match) - 1;
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

array<int, 63> points_per_match{
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

int input(int index)
{
   Round round_details = round_index(index + 1);
   int inputRound = round_details.round + 1; // + 1 for previous round.
   int inputRoundIndex = round_details.index * 2 - 1;
   return match(inputRound, inputRoundIndex) - 1;
}

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

scoretuple_t random_scoretuple()
{
   scoretuple_t result = 0;
   for (size_t i = 0; i < NUM_BRACKETS; i++)
   {
      result = result * 256 + randint(81);
   }
   return result;
}

struct Matchup
{
   game_t id = -1;
   team_t first_team = -1; // Should really use std::optional for these.
   team_t second_team = -1;
   team_t winner = -1;
};

vector<string> teams(64);

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

vector<bitset<64>> all_selections(NUM_GAMES);

Bracket get_bracket(uint64_t entry)
{
   Bracket result;
   string html = get_entry(entry);
   result.name = get_json("espn.fantasy.maxpart.config.Entry", html)["n_e"];
   if (result.name.starts_with("Owe"))
   {
      result.name = "Owe'n Charlie '22";
   }
   if (result.name.starts_with("Maureen"))
   {
      result.name = "Maureen's Annual Bonus";
   }
   auto picks_str = get_json("espn.fantasy.maxpart.config.pickString", html);
   stringstream stream(picks_str.get<string>());
   string item;
   while (getline(stream, item, '|'))
   {
      result.picks.push_back(stoi(item) - 1);
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

scoretuple_t get_scoretuple(game_t match_index, team_t winning_team, uint8_t reduced_points)
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
   unordered_set<scoretuple_t> scores;

   Outcomes(team_t team, scoretuple_t scores) : team(team), scores{scores} {}
   Outcomes() : team(-1) {}
   Outcomes(team_t team) : team(team) {}
};

string to_string(const Outcomes &outcome)
{
   string result;
   result += (outcome.team < 0 ? "other" : teams[outcome.team]);
   for (const auto scores : outcome.scores)
   {
      result += " " + make_string(scores);
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

// The first element of the vector is always for team "other".
vector<Outcomes> outcomes(
    game_t match_index,
    bitset<64> selections)
{
   const auto ri = round_index(match_index + 1);
   int this_points = points_per_match[match_index];
   if (ri.round == 5)
   {
      // Base case, round of 64.
      myassert(this_points == 10);
      vector<Outcomes> result(1);
      const Matchup &game = games[match_index];
      vector<team_t> teams;
      if (game.winner >= 0)
      {
         teams.push_back(game.winner);
      }
      else
      {
         myassert(game.first_team >= 0);
         myassert(game.second_team >= 0);
         teams.push_back(game.first_team);
         teams.push_back(game.second_team);
      }

      for (team_t team : teams)
      {
         auto scores = get_scoretuple(match_index, team, this_points / 10);
         myassert(team >= 0);
         if (selections[team])
         {
            result.emplace_back(team, scores);
         }
         else
         {
            result[0].scores.insert(scores);
         }
      }

      return result;
   }

   // General case.  Start by recursing.
   int prev_match = input(match_index);
   const auto outcomes1 = outcomes(prev_match, all_selections[match_index]);
   const auto outcomes2 = outcomes(prev_match + 1, all_selections[match_index]);
   vector<Outcomes> result(1);
   cout << "About to loop, round " << ri.round << endl;

   for (const auto &outcome1 : outcomes1)
   {
      for (const auto &outcome2 : outcomes2)
      {
         for (auto winner : {outcome1.team, outcome2.team})
         {
            // Find the destination spot in result
            Outcomes *dest;
            if (winner < 0 || !selections[winner])
            {
               dest = &result[0];
            }
            else
            {
               dest = find_team(result, winner);
               if (!dest)
               {
                  result.emplace_back(winner);
                  dest = &result[result.size() - 1];
               }
            }
            for (const scoretuple_t score1 : outcome1.scores)
            {
               for (const scoretuple_t score2 : outcome2.scores)
               {
                  scoretuple_t total_scores = score1 + score2;
                  scoretuple_t overall_scores;
                  if (winner < 0)
                  {
                     overall_scores = total_scores;
                  }
                  else
                  {
                     auto this_scores = get_scoretuple(match_index, winner, this_points / 10);
                     overall_scores = total_scores + this_scores;
                  }
                  dest->scores.insert(overall_scores);
               }
            }
         }
      }
   }

   cout << "loop done.\n";
   return result;
}

int main(int argc, char *argv[])
{
   string html = get_entry(entries[0]);
   const auto teams_json = get_json("espn.fantasy.maxpart.config.scoreboard_teams", html);
   myassert(teams_json.size() == 64);
   for (const auto &team : teams_json)
   {
      teams[team["id"].get<int>() - 1] = team["n"];
   }

   const auto matchups_json = get_json("espn.fantasy.maxpart.config.scoreboard_matchups", html);

   myassert(matchups_json.size() == 63);
   for (const auto &matchup : matchups_json)
   {
      Matchup m = parse_matchup(matchup);
      games[m.id] = m;
   }

   for (auto entry : entries)
   {
      brackets.push_back(get_bracket(entry));
   }

   make_all_selections();

   for (int match_index = 0; match_index < 63; match_index++)
   {
      cout << match_index;
      for (size_t team_index = 0; team_index < 64; ++team_index)
      {
         if (all_selections[match_index][team_index])
         {
            cout << ", " << teams[team_index];
         }
      }
      cout << "\n";
   }

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

   return 0;

   vector<scoretuple_t> other1;
   for (size_t i = 0; i < NUM_OTHER1; i++)
   {
      other1.push_back(random_scoretuple());
   }

   vector<scoretuple_t> other2;
   for (size_t i = 0; i < NUM_OTHER2; i++)
   {
      other2.push_back(random_scoretuple());
   }

   cout << "Starting loop." << endl;
   auto start = high_resolution_clock::now();
   int i = 0;
   vector<size_t> counts(NUM_BRACKETS);
   for (auto o1 : other1)
   {
      for (auto o2 : other2)
      {
         const scoretuple_t total = o1 + o2;

         counts[winner(total)]++;
      }
      i++;
      if (i % 500 == 0)
      {
         double elapsed = duration_cast<microseconds>(high_resolution_clock::now() - start).count() / 1e6;

         cout << elapsed / i * other1.size() << " " << elapsed / i * (other1.size() - i) << " sec\n";
      }
   }

   for (size_t i = 0; i < NUM_BRACKETS; i++)
   {
      cout << counts[i] << "\n";
   }

   return 0;
}
