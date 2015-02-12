#include <iostream>
#include <chrono>
#include <fstream>
#include <boost/program_options.hpp>
#include "cfrm.hpp"
#include "functions.hpp"

extern "C" {
#include "net.h"
}

using namespace std;
namespace ch = std::chrono;
namespace po = boost::program_options;

int parse_options(int argc, char **argv);
void read_game(char *game_definition);

struct {
  string game_definition = "games/holdem.limit.2p.reverse_blinds.game";

  string host = "localhost";
  unsigned port = 18791;

  unsigned seed = 0;

  card_abstraction card_abs = CLUSTERCARD_ABS;
  action_abstraction action_abs = NULLACTION_ABS;
  string card_abs_param = "";
  string action_abs_param = "";

  string init_strategy = "";
} options;

const Game *gamedef;

int main(int argc, char **argv) {
  int sock, len, r, a;
  int32_t min, max;
  uint16_t port;
  double p;
  MatchState state;
  Action action;
  FILE *file, *toServer, *fromServer;
  struct timeval tv;
  double probs[NUM_ACTION_TYPES];
  double actionProbs[NUM_ACTION_TYPES];
  char line[MAX_LINE_LEN];

  /* Initialize the player's random number state using time */

  if (parse_options(argc, argv) == 1)
    return 1;

  nbgen rng(options.seed);
  read_game((char *)options.game_definition.c_str());

  CardAbstraction *cardabs;
  ActionAbstraction *actionabs;
  switch (options.card_abs) {
  case NULLCARD_ABS:
    cardabs = new NullCardAbstraction(gamedef, "");
    break;
  case BLINDCARD_ABS:
    cardabs = new BlindCardAbstraction(gamedef, "");
    break;
  case CLUSTERCARD_ABS:
    cardabs = new ClusterCardAbstraction(gamedef, "");
    break;
  default:
    return NULL;
    // throw
  };

  switch (options.action_abs) {
  case NULLACTION_ABS:
    actionabs = new NullActionAbstraction(gamedef,"");
    break;
  default:
    // throw
    return NULL;
  };

  AbstractGame *agame = new HoldemGame(gamedef, cardabs, actionabs, NULL);
  CFRM *cfr =
      new ExternalSamplingCFR(agame, (char *)options.init_strategy.c_str());

  /* connect to the dealer */
  sock = connectTo((char *)options.host.c_str(), options.port);
  if (sock < 0) {

    exit(EXIT_FAILURE);
  }
  toServer = fdopen(sock, "w");
  fromServer = fdopen(sock, "r");
  if (toServer == NULL || fromServer == NULL) {

    fprintf(stderr, "ERROR: could not get socket streams\n");
    exit(EXIT_FAILURE);
  }

  //[> send version string to dealer <]
  if (fprintf(toServer, "VERSION:%" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
              VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION) != 14) {

    fprintf(stderr, "ERROR: could not get send version to server\n");
    exit(EXIT_FAILURE);
  }
  fflush(toServer);

  // play the gamedef!
  while (fgets(line, MAX_LINE_LEN, fromServer)) {
    // line = "MATCHSTATE:1:994:r:|Ks";
    // std::cout << line << "\n";

    /* ignore comments */
    if (line[0] == '#' || line[0] == ';') {
      continue;
    }

    len = readMatchState(line, gamedef, &state);
    if (len < 0) {

      fprintf(stderr, "ERROR: could not read state %s", line);
      exit(EXIT_FAILURE);
    }

    if (stateFinished(&state.state)) {
      /* ignore the gamedef over message */
      continue;
    }

    if (currentPlayer(gamedef, &state.state) != state.viewingPlayer) {
      /* we're not acting */
      continue;
    }

    /* add a colon (guaranteed to fit because we read a new-line in fgets) */
    line[len] = ':';
    ++len;

     //char* c = new char[100];
     //printState(gamedef, &state.state, 100, c);
     //printf("%s\n",c);
    // int d = state.state.holeCards[state.viewingPlayer][0];

    // std::cout << "hand: " << d  << "\n";

    // lookup current node we are in
    InformationSetNode *curr_node = (InformationSetNode *)cfr->lookup_state(
        &state.state, state.viewingPlayer);
    // get strategy at curr_node
    //
    card_c hand(gamedef->numHoleCards);
    for (int i = 0; i < gamedef->numHoleCards; ++i) {
      hand[i] = state.state.holeCards[state.viewingPlayer][i];
    }

    card_c board;
    //for (int i = 0; i < gamedef->numRounds; ++i) {
      //if (i > state.state.round)
        //continue;
      for (int c = 0; c < sumBoardCards(gamedef,state.state.round); ++c) {
        board.push_back(state.state.boardCards[c]);
      }
    //}
    //std::cout << int(hand[0]) << "," << int(hand[1]) << "\n";
    //for(unsigned i = 0; i < board.size(); ++i)
        //std::cout << int(board[i]) << " ";
    //std::cout << "\n";

    auto strategy = cfr->get_normalized_avg_strategy(curr_node->get_idx(), hand,
                                                     board, state.state.round);

    // choose according to distribution

    // for( a = 0; a < NUM_ACTION_TYPES; ++a ) {
    std::discrete_distribution<int> d(strategy.begin(), strategy.end());
    int action_idx = d(rng);
    action = curr_node->get_children()[action_idx]->get_action();

    //std::cout << ActionsStr[action.type] << " = " << action.size << "\n";

    //[> do the action! <]
    //if(action.type == a_fold){
        //std::cout << "fold action. ";
        //auto oh = action;
        //oh.type = a_check;
        //if(isValidAction(gamedef, &state.state, 0, &oh))
            //std::cout << " check is possible\n";
    //}

    assert(isValidAction(gamedef, &state.state, 0, &action));
    r = printAction(gamedef, &action, MAX_LINE_LEN - len - 2, &line[len]);
    if (r < 0) {

      fprintf(stderr, "ERROR: line too long after printing action\n");
      exit(EXIT_FAILURE);
    }
    len += r;
    line[len] = '\r';
    ++len;
    line[len] = '\n';
    ++len;

    if (fwrite(line, 1, len, toServer) != len) {

      fprintf(stderr, "ERROR: could not get send response to server\n");
      exit(EXIT_FAILURE);
    }
    fflush(toServer);
  }

  return 0;
}

int parse_options(int argc, char **argv) {
  try {
    po::options_description desc("Allowed options");
    desc.add_options()("help,h", "produce help message")(
        "card-abstraction,c", po::value<string>(),
        "set card abstraction to use")("action-abstraction,a",
                                       po::value<string>(),
                                       "set action abstraction to use")(
        "card-abs-param,m", po::value<string>(&options.card_abs_param), "parameter for card abstraction")(
        "host,o", po::value<string>(&options.host), "host to connect to")(
        "port,p", po::value<unsigned>(&options.port), "port to connect to")(
        "init-stategy,i", po::value<string>(&options.init_strategy),
        "initialize regrets and avg strategy from file")(
        "gamedef,g", po::value<string>(&options.game_definition),
        "gamedefinition to use");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("card-abstraction")) {
      string ca = vm["card-abstraction"].as<string>();
      if (ca == "null")
        options.card_abs = NULLCARD_ABS;
      else if (ca == "cluster")
        options.card_abs = CLUSTERCARD_ABS;
    }

    if (vm.count("action-abstraction")) {
      string ca = vm["action-abstraction"].as<string>();
      if (ca == "null")
        options.action_abs = NULLACTION_ABS;
      else if (ca == "potrel")
        options.action_abs = POTRELACTION_ABS;
    }

    if (vm.count("help")) {
      cout << desc << "\n";
      return 1;
    }

    // if (vm.count("handranks")) {
    // options.handranks_file = vm["handranks"].as<string>();
    //}
  }
  catch (exception &e) {
  }
  catch (...) {
  }
  return 0;
}

void read_game(char *game_definition) {
  FILE *file = fopen(game_definition, "r");
  if (file == NULL) {
    std::cout << "could not read game file\n";
    exit(-1);
  }
  gamedef = readGame(file);
  if (gamedef == NULL) {
    std::cout << "could not parse game file\n";
    exit(-1);
  }
}
