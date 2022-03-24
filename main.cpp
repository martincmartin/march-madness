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

const array<const string, NUM_ROUNDS> round_names{
    "Champ",
    "Final4",
    "Elite8",
    "Sweet16",
    "Roundof32",
    "Roundof64",
};

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
      myassert(teams[result.first_team].name == match_teams[0]["n"]);
   }
   if (match_teams.size() >= 2)
   {
      result.second_team = match_teams[1]["id"].get<int>() - 65;
      myassert(teams[result.second_team].name == match_teams[1]["n"]);
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

   /*
      void add(shared_ptr<BoolExpr> child)
      {
         if (children.find(child) != children.end())
         {
            return;
         }
         if (auto var = reinterpret_cast<Var *>(child.get()))
         {
            // Find it's negated twin.
            const auto &temp = Var::all_vars[var->index];
            myassert((var->first ? temp.first : temp.second).get() == var);
            const shared_ptr<Var> &twin = var->first ? temp.second : temp.first;
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
      */
};

class AndExpr : public InfixExpr
{
public:
   AndExpr(unordered_set<shared_ptr<BoolExpr>> kids) : InfixExpr{std::move(kids)} {}

   pair<string, string> empty_and_operation() const
   {
      return make_pair(string("true"), string("and"));
   };

   /*
      void add(shared_ptr<BoolExpr> child)
      {
         children.insert(child);
      }
      */
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
                        myassert(new_dual_children.size() >= 1);
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

struct ResultSet
{
   double prob;
   shared_ptr<BoolExpr> which;

   void combine_disjoint(const ResultSet other)
   {
      prob += other.prob;
      which = or_({which, other.which});
   }
};

string to_string(ResultSet result_set)
{
   return fmt::format("{:.3f}% ", result_set.prob * 100) + to_string(result_set.which);
}

struct Outcomes
{
   team_t team;
   unordered_map<scoretuple_t, ResultSet> result_sets;

   Outcomes(team_t team, scoretuple_t scores, ResultSet set) : team(team), result_sets{{scores, set}} {}
   Outcomes() : team(-1) {}
   Outcomes(team_t team) : team(team) {}
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

struct TeamInfo
{
   team_t team;
   ResultSet result_set;
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
   if (ri.round == NUM_ROUNDS - 1)
   {
      // Base case, round of 64.
      myassert(this_points == 10);
      vector<Outcomes> result(1);
      vector<TeamInfo> teams_with_probs;
      if (game.winner >= 0)
      {
         teams_with_probs.push_back({game.winner, {1.0, {}}});
      }
      else
      {
         myassert(game.first_team >= 0);
         myassert(game.second_team >= 0);
         double prob_first = game_prob(game.first_team, game.second_team, game.first_team, ri.round);
         teams_with_probs.push_back({game.first_team, {prob_first, Var::all_vars[game.id].first}});
         teams_with_probs.push_back({game.second_team, {1.0 - prob_first, Var::all_vars[game.id].second}});
      }

      for (const TeamInfo &team_with_prob : teams_with_probs)
      {
         auto scores = get_scoretuple(match_index, team_with_prob.team, this_points / 10);
         myassert(team_with_prob.team >= 0);

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
   const auto outcomes1 = outcomes(prev_match, all_selections[match_index]);
   const auto outcomes2 = outcomes(prev_match + 1, all_selections[match_index]);
   vector<Outcomes> result(1);
   if (ri.round <= 1)
   {
      cout << "About to loop, round " << ri.round << endl;
   }

   for (const Outcomes &outcome1 : outcomes1)
   {
      if (outcome1.result_sets.empty())
      {
         continue;
      }
      myassert(outcome1.team >= 0);
      /*
      if (ri.round == 0)
      {
         */
      // cout << (outcome1.team < 0 ? "other" : teams[outcome1.team].name) << endl;
      /*
   }
   */
      for (const Outcomes &outcome2 : outcomes2)
      {
         if (outcome2.result_sets.empty())
         {
            continue;
         }
         myassert(outcome2.team >= 0);

         /*
         if (ri.round == 0)
         {*/
         // cout << "    " << (outcome2.team < 0 ? "other" : teams[outcome2.team].name) << endl;
         /*
      }
      */

         vector<TeamInfo> teams_with_probs;
         if (game.winner >= 0)
         {
            // If this game has been played in real life, then all previous games
            // have also been played, so our recursive outcomes had better have
            // only a single non-empty result.
            myassert(outcomes1.size() == 1 || (outcomes1.size() == 2 && outcomes1[0].result_sets.empty()));
            myassert(outcomes2.size() == 1 || (outcomes2.size() == 2 && outcomes2[0].result_sets.empty()));
            myassert(game.winner == outcome1.team || game.winner == outcome2.team);
            teams_with_probs.push_back({game.winner, {1.0, {}}});
         }
         else
         {
            double prob_first = game_prob(outcome1.team, outcome2.team, outcome1.team, ri.round);
            teams_with_probs.push_back({outcome1.team, {prob_first, Var::all_vars[match_index].first}});
            teams_with_probs.push_back({outcome2.team, {1.0 - prob_first, Var::all_vars[match_index].second}});
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

            for (const auto &score_and_prob1 : outcome1.result_sets)
            {
               for (const auto &score_and_prob2 : outcome2.result_sets)
               {
                  scoretuple_t total_scores = score_and_prob1.first + score_and_prob2.first;
                  scoretuple_t overall_scores;
                  ResultSet new_set;

                  if (winner.team < 0)
                  {
                     overall_scores = total_scores;
                     new_set.which = and_({score_and_prob1.second.which, score_and_prob2.second.which});
                  }
                  else
                  {
                     new_set.which = and_({score_and_prob1.second.which, score_and_prob2.second.which, winner.result_set.which});
                     auto this_scores = get_scoretuple(match_index, winner.team, this_points / 10);
                     overall_scores = total_scores + this_scores;
                  }
                  // This is where I do the "or" with existing results.;
                  ResultSet &rset = dest->result_sets[normalize(overall_scores)];
                  new_set.prob = winner.result_set.prob * score_and_prob1.second.prob * score_and_prob2.second.prob;
                  /*
                  if (rset.which)
                  {
                     cout << "Oring " << to_string(rset.which) << " *WITH* " << to_string(new_set.which) << "\n";
                     cout << "  Got " << to_string(or_({rset.which, new_set.which})) << "\n";
                  }
                  */
                  rset.combine_disjoint(new_set);
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
      teams[id].name = team["n"];
      teams[id].abbrev = team["a"];
      eid_to_team[team["eid"].get<int>()] = id;
   }

   const auto matchups_json = get_json("espn.fantasy.maxpart.config.scoreboard_matchups", html);

   myassert(matchups_json.size() == NUM_GAMES);
   for (const auto &matchup : matchups_json)
   {
      Matchup m = parse_matchup(matchup);
      games[m.id] = m;
      if (m.winner < 0)
      {
         const string &rname = round_names[round_index(m.id + 1).round];
         const string first_name = (m.first_team < 0 ? to_string(m.id) + "first" : teams[m.first_team].abbrev);
         const string second_name = (m.second_team < 0 ? to_string(m.id) + "second" : teams[m.second_team].abbrev);
         Var::all_vars[m.id] = make_pair(make_shared<Var>(first_name + "-" + rname, m.id, true),
                                         make_shared<Var>(second_name + "-" + rname, m.id, false));
      }
   }

   for (auto entry : entries)
   {
      brackets.push_back(get_bracket(entry));
   }

   myassert(brackets.size() == NUM_BRACKETS);

   make_all_selections();

   myassert(all_selections.size() == NUM_GAMES);

   parse_probs();

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
#endif

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

   {
      cout << "**********  Whole Thing!\n";
      auto start = high_resolution_clock::now();
      auto results = outcomes(62, {});
      cout << "Elapsed " << elapsed(start, high_resolution_clock::now()) << " sec.\n";

      for (const auto &outcome : results)
      {
         if (!outcome.result_sets.empty())
         {
            cout << to_string(outcome);

            // cout << (outcome.team < 0 ? "other" : teams[outcome.team].name) << ": " << outcome.result_sets.size() << "\n";
         }
      }

      // return 0;

      vector<pair<int, ResultSet>> win_probs(NUM_BRACKETS);
      for (size_t i = 0; i < NUM_BRACKETS; ++i)
      {
         win_probs[i].first = i;
      }

      for (const Outcomes &outc : results)
      {
         for (const auto &score_and_result_sets : outc.result_sets)
         {
            win_probs[winner(score_and_result_sets.first)].second.combine_disjoint(
                score_and_result_sets.second);
         }
      }

      cout << "***** Probability of Win for each Bracket *****\n";
      sort(win_probs.begin(), win_probs.end(), [](auto &a, auto &b)
           { return a.second.prob > b.second.prob; });
      for (int i = 0; i < NUM_BRACKETS; ++i)
      {
         cout << fmt::format("{:<22}: {:5.2f}% ", brackets[win_probs[i].first].name, win_probs[i].second.prob * 100) << (win_probs[i].second.which)->sexpr(0) << "\n";
      }
   }

   return 0;
}
