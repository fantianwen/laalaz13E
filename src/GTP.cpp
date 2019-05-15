/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2018 Gian-Carlo Pascutto and contributors

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <vector>
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>

#include "GTP.h"
#include "FastBoard.h"
#include "FullBoard.h"
#include "GameState.h"
#include "Network.h"
#include "SGFTree.h"
#include "SMP.h"
#include "Training.h"
#include "UCTSearch.h"
#include "Utils.h"
#include "UCTNodePointer.h"

using namespace Utils;

// Configuration flags
bool cfg_gtp_mode;
bool cfg_allow_pondering;
int cfg_num_threads;
int cfg_max_threads;
int cfg_max_playouts;
int cfg_max_visits;
float alpha;
int currentMoveNumber;
size_t cfg_max_memory;
size_t cfg_max_tree_size;
int cfg_max_cache_ratio_percent;
TimeManagement::enabled_t cfg_timemanage;
int cfg_lagbuffer_cs;
int cfg_resignpct;
int cfg_noise;
int cfg_random_cnt;
int cfg_random_min_visits;
float cfg_random_temp;
std::uint64_t cfg_rng_seed;
bool cfg_dumbpass;
#ifdef USE_OPENCL
std::vector<int> cfg_gpus;
bool cfg_sgemm_exhaustive;
bool cfg_tune_only;
#ifdef USE_HALF
precision_t cfg_precision;
#endif
#endif
float cfg_puct;
float cfg_softmax_temp;
float cfg_fpu_reduction;
float cfg_fpu_root_reduction;
std::string cfg_weightsfile;
std::string cfg_weightsfile_s;
std::string cfg_logfile;
FILE* cfg_logfile_handle;
bool cfg_quiet;
std::string cfg_options_str;
bool cfg_benchmark;
bool cfg_cpu_only;
int cfg_analyze_interval_centis;
int last_move;

std::unique_ptr<Network> GTP::s_network;
std::unique_ptr<Network> GTP::s_network_s;

void GTP::initialize(std::unique_ptr<Network>&& net,std::unique_ptr<Network>&& net_s) {
    s_network = std::move(net);
    s_network_s = std::move(net_s);

    bool result;
    std::string message;
    std::tie(result, message) =
            set_max_memory(cfg_max_memory, cfg_max_cache_ratio_percent);
    if (!result) {
        // This should only ever happen with 60 block networks on 32bit machine.
        myprintf("LOW MEMORY SETTINGS! Couldn't set default memory limits.\n");
        myprintf("The network you are using might be too big\n");
        myprintf("for the default settings on your system.\n");
        throw std::runtime_error("Error setting memory requirements.");
    }
    myprintf(message.c_str());
    myprintf("\n");
}

void GTP::setup_default_parameters() {
    cfg_gtp_mode = false;
    cfg_allow_pondering = true;
    cfg_max_threads = std::max(1, std::min(SMP::get_num_cpus(), MAX_CPUS));
#ifdef USE_OPENCL
    // If we will be GPU limited, using many threads won't help much.
    // Multi-GPU is a different story, but we will assume that those people
    // who do those stuff will know what they are doing.
    cfg_num_threads = std::min(2, cfg_max_threads);
#else
    cfg_num_threads = cfg_max_threads;
#endif
    cfg_max_memory = UCTSearch::DEFAULT_MAX_MEMORY;
    cfg_max_playouts = UCTSearch::UNLIMITED_PLAYOUTS;
    cfg_max_visits = UCTSearch::UNLIMITED_PLAYOUTS;
    // This will be overwriiten in initialize() after network size is known.
    cfg_max_tree_size = UCTSearch::DEFAULT_MAX_MEMORY;
    cfg_max_cache_ratio_percent = 10;
    cfg_timemanage = TimeManagement::AUTO;
    cfg_lagbuffer_cs = 100;
    cfg_weightsfile = leelaz_file("best-network");
    cfg_weightsfile_s = cfg_weightsfile;
#ifdef USE_OPENCL
    cfg_gpus = { };
    cfg_sgemm_exhaustive = false;
    cfg_tune_only = false;
#ifdef USE_HALF
    cfg_precision = precision_t::AUTO;
#endif
#endif
    cfg_puct = 0.8f;
    cfg_softmax_temp = 1.0f;
    cfg_fpu_reduction = 0.25f;
    // see UCTSearch::should_resign
    cfg_resignpct = -1;
    cfg_noise = false;
    cfg_fpu_root_reduction = cfg_fpu_reduction;
    cfg_random_cnt = 0;
    cfg_random_min_visits = 1;
    cfg_random_temp = 1.0f;
    cfg_dumbpass = false;
    cfg_logfile_handle = nullptr;
    cfg_quiet = false;
    cfg_benchmark = false;
#ifdef USE_CPU_ONLY
    cfg_cpu_only = true;
#else
    cfg_cpu_only = false;
#endif

    cfg_analyze_interval_centis = 0;

    // C++11 doesn't guarantee *anything* about how random this is,
    // and in MinGW it isn't random at all. But we can mix it in, which
    // helps when it *is* high quality (Linux, MSVC).
    std::random_device rd;
    std::ranlux48 gen(rd());
    std::uint64_t seed1 = (gen() << 16) ^ gen();
    // If the above fails, this is one of our best, portable, bets.
    std::uint64_t seed2 = std::chrono::high_resolution_clock::
    now().time_since_epoch().count();
    cfg_rng_seed = seed1 ^ seed2;
}

const std::string GTP::s_commands[] = {
        "protocol_version",
        "name",
        "version",
        "quit",
        "known_command",
        "list_commands",
        "boardsize",
        "clear_board",
        "komi",
        "play",
        "genmove",
        "showboard",
        "undo",
        "final_score",
        "final_status_list",
        "time_settings",
        "time_left",
        "fixed_handicap",
        "place_free_handicap",
        "set_free_handicap",
        "loadsgf",
        "printsgf",
        "kgs-genmove_cleanup",
        "kgs-time_settings",
        "kgs-game_over",
        "heatmap",
        "lz-analyze",
        "lz-genmove_analyze",
        "lz-memory_report",
        "lz-setoption",
        "autotrain",
        "check_running",
        "lastMove"
        ""
};

// Default/min/max could be moved into separate fields,
// but for now we assume that the GUI will not send us invalid info.
const std::string GTP::s_options[] = {
        "option name Maximum Memory Use (MiB) type spin default 2048 min 128 max 131072",
        "option name Percentage of memory for cache type spin default 10 min 1 max 99",
        "option name Visits type spin default 0 min 0 max 1000000000",
        "option name Playouts type spin default 0 min 0 max 1000000000",
        "option name Lagbuffer type spin default 0 min 0 max 3000",
        "option name Resign Percentage type spin default -1 min -1 max 30",
        "option name Pondering type check default true",
        ""
};

std::string GTP::get_life_list(const GameState & game, bool live) {
    std::vector<std::string> stringlist;
    std::string result;
    const auto& board = game.board;

    if (live) {
        for (int i = 0; i < board.get_boardsize(); i++) {
            for (int j = 0; j < board.get_boardsize(); j++) {
                int vertex = board.get_vertex(i, j);

                if (board.get_state(vertex) != FastBoard::EMPTY) {
                    stringlist.push_back(board.get_string(vertex));
                }
            }
        }
    }

    // remove multiple mentions of the same string
    // unique reorders and returns new iterator, erase actually deletes
    std::sort(begin(stringlist), end(stringlist));
    stringlist.erase(std::unique(begin(stringlist), end(stringlist)),
                     end(stringlist));

    for (size_t i = 0; i < stringlist.size(); i++) {
        result += (i == 0 ? "" : "\n") + stringlist[i];
    }

    return result;
}

void GTP::execute(GameState & game, const std::string& xinput) {
    std::string input;
    static auto search = std::make_unique<UCTSearch>(game, *s_network);
    static auto search_s = std::make_unique<UCTSearch>(game, *s_network_s);

    bool transform_lowercase = true;

    // Required on Unixy systems
    if (xinput.find("loadsgf") != std::string::npos) {
        transform_lowercase = false;
    }

    /* eat empty lines, simple preprocessing, lower case */
    for (unsigned int tmp = 0; tmp < xinput.size(); tmp++) {
        if (xinput[tmp] == 9) {
            input += " ";
        } else if ((xinput[tmp] > 0 && xinput[tmp] <= 9)
                   || (xinput[tmp] >= 11 && xinput[tmp] <= 31)
                   || xinput[tmp] == 127) {
            continue;
        } else {
            if (transform_lowercase) {
                input += std::tolower(xinput[tmp]);
            } else {
                input += xinput[tmp];
            }
        }

        // eat multi whitespace
        if (input.size() > 1) {
            if (std::isspace(input[input.size() - 2]) &&
                std::isspace(input[input.size() - 1])) {
                input.resize(input.size() - 1);
            }
        }
    }

    std::string command;
    int id = -1;

    if (input == "") {
        return;
    } else if (input == "exit") {
        exit(EXIT_SUCCESS);
    } else if (input.find("#") == 0) {
        return;
    } else if (std::isdigit(input[0])) {
        std::istringstream strm(input);
        char spacer;
        strm >> id;
        strm >> std::noskipws >> spacer;
        std::getline(strm, command);
    } else {
        command = input;
    }

    /* process commands */
    if (command == "protocol_version") {
        gtp_printf(id, "%d", GTP_VERSION);
        return;
    } else if (command == "name") {
        gtp_printf(id, PROGRAM_NAME);
        return;
    } else if (command == "version") {
        gtp_printf(id, PROGRAM_VERSION);
        return;
    } else if (command == "quit") {
        gtp_printf(id, "");
        exit(EXIT_SUCCESS);
    } else if (command.find("known_command") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;

        cmdstream >> tmp;     /* remove known_command */
        cmdstream >> tmp;

        for (int i = 0; s_commands[i].size() > 0; i++) {
            if (tmp == s_commands[i]) {
                gtp_printf(id, "true");
                return;
            }
        }

        gtp_printf(id, "false");
        return;
    } else if (command.find("list_commands") == 0) {
        std::string outtmp(s_commands[0]);
        for (int i = 1; s_commands[i].size() > 0; i++) {
            outtmp = outtmp + "\n" + s_commands[i];
        }
        gtp_printf(id, outtmp.c_str());
        return;
    } else if (command.find("boardsize") == 0) {
        std::istringstream cmdstream(command);
        std::string stmp;
        int tmp;

        cmdstream >> stmp;  // eat boardsize
        cmdstream >> tmp;

        if (!cmdstream.fail()) {
            if (tmp != BOARD_SIZE) {
                gtp_fail_printf(id, "unacceptable size");
            } else {
                float old_komi = game.get_komi();
                Training::clear_training();
                game.init_game(tmp, old_komi);
                gtp_printf(id, "");
            }
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }

        return;
    } else if (command.find("clear_board") == 0) {
        Training::clear_training();
        game.reset_game();
        search = std::make_unique<UCTSearch>(game, *s_network);
        assert(UCTNodePointer::get_tree_size() == 0);
        gtp_printf(id, "");
        return;
    } else if (command.find("komi") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        float komi = 7.5f;
        float old_komi = game.get_komi();

        cmdstream >> tmp;  // eat komi
        cmdstream >> komi;

        if (!cmdstream.fail()) {
            if (komi != old_komi) {
                game.set_komi(komi);
            }
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }

        return;
    } else if (command.find("play") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        std::string color, vertex;

        cmdstream >> tmp;   //eat play
        cmdstream >> color;
        cmdstream >> vertex;

        if (!cmdstream.fail()) {
            if (!game.play_textmove(color, vertex)) {
                gtp_fail_printf(id, "illegal move");
            } else {
                gtp_printf(id, "");
            }
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        return;
    } else if (command.find("genmove") == 0
               || command.find("lz-genmove_analyze") == 0) {
        auto analysis_output = command.find("lz-genmove_analyze") == 0;
        auto interval = 0;

        currentMoveNumber++;

        std::istringstream cmdstream(command);
        std::string tmp;

        cmdstream >> tmp;  // eat genmove
        cmdstream >> tmp;
        if (analysis_output) {
            cmdstream >> interval;
        }

        if (!cmdstream.fail()) {
            int who;
            if (tmp == "w" || tmp == "white") {
                who = FastBoard::WHITE;
            } else if (tmp == "b" || tmp == "black") {
                who = FastBoard::BLACK;
            } else {
                gtp_fail_printf(id, "syntax error");
                return;
            }
            if (analysis_output) {
                // Start of multi-line response
                cfg_analyze_interval_centis = interval;
                if (id != -1) gtp_printf_raw("=%d\n", id);
                else gtp_printf_raw("=\n");
            }
            // start thinking
            {
                game.set_to_move(who);
                // Outputs winrate and pvs for lz-genmove_analyze
                //=======================888=======================

                printf("begin to show candidates moves \n");

                std::string mixed_info = "C[";

                mixed_info+="vertex\twr\tvisit\tsp\ts_sp\n";

                std::vector<UCTNodePointer>& candidates = search->think_s(who);
                std::vector<UCTNodePointer>& candidates_s = search_s->think_s(who);

                int total_count_candidates =0;

                int total_count_candidates_s =0;

                for (const auto& child : candidates) {
                    total_count_candidates+=child.get_visits();
                }

                for (const auto& child_s : candidates_s) {
                    total_count_candidates_s+=child_s.get_visits();
                }

                printf("the total visit count is: %d and %d",total_count_candidates,total_count_candidates_s);

                int selected_move = 0;
                float mixed_eval = 0;

                if (candidates.size()<=0 || candidates_s.size()<=0) {
                    selected_move = FastBoard::PASS;
                    game.play_move(who, selected_move,"");
                }else{
//                    for (const auto& child_s : candidates_s) {
//
//                        float temp_mix_eval;
//                        int temp_move;
//
//                        float eval_s = (float)child_s.get_visits()/total_count_candidates_s;
//                        int move_s = child_s.get_move();
//
//                        printf("the eval is %f",eval_s);
//
//                        for (const auto& child : candidates){
//
//                            float eval = (float)child.get_visits()/total_count_candidates;
//                            int move = child.get_move();
//
//                            if(move == move_s){
//                                temp_mix_eval = eval_s*alpha+eval*(1-alpha);
//                                break;
//                            }else{
//                                temp_mix_eval = eval_s*alpha;
//                            }
//                        }
//
//                        temp_move = move_s;
//
//                        if(temp_mix_eval>0){
//                            std::string movestr = Utils::convertVertex(temp_move);
//                            std::string::size_type idx;
//                            idx = mixed_info.find(movestr);
//                            if(idx==std::string::npos){
//                                mixed_info+=Utils::convertVertex(temp_move)+"\t"+std::to_string(temp_mix_eval)+"\n";
//                            }
//                        }
//
//                        if(temp_mix_eval>mixed_eval){
//                            mixed_eval = temp_mix_eval;
//                            selected_move = temp_move;
//                        }
//
//                    }

//                    for (const auto& child : candidates) {
//
//                        float temp_mix_eval ;
//                        int temp_move;
//
//                        float eval = (float)child.get_visits()/total_count_candidates;
//
//                        int move = child.get_move();
//
//                        for (const auto& child_s : candidates_s){
//
//                            float eval_s = (float)child_s.get_visits()/total_count_candidates_s;
//                            int move_s = child_s.get_move();
//
//                            if(move == move_s){
//                                temp_mix_eval = eval_s*alpha+eval*(1-alpha);
//                                break;
//                            }else{
//                                temp_mix_eval = eval*(1-alpha);
//                            }
//                        }
//
//                        temp_move = move;
//
//                        if(temp_mix_eval>0){
//                            std::string movestr = Utils::convertVertex(temp_move);
//                            std::string::size_type idx;
//                            idx = mixed_info.find(movestr);
//                            if(idx==std::string::npos){
//                                mixed_info+=Utils::convertVertex(temp_move)+"\t"+std::to_string(temp_mix_eval)+"\n";
//                            }
//                        }
//
//
//                        if(temp_mix_eval>mixed_eval){
//                            mixed_eval = temp_mix_eval;
//                            selected_move = temp_move;
//                        }
//
//                    }

                    printf("the mixed eval is :%f",mixed_eval);

                    if(mixed_eval<0.01){
                        selected_move = FastBoard::PASS;
                    }

                    if (currentMoveNumber<=2){
                        selected_move = candidates.front().get_move();
                    }else {
                        selected_move = candidates_s.front().get_move();
                    }

                    std::string last_comments = search->get_last_comments(who);

                    std::string last_comments_s = search_s->get_last_comments(who);

                    mixed_info += last_comments_s+"\n"+last_comments+"\n";

                    mixed_info+="]";

                    game.play_move(who, selected_move,mixed_info);
                }

//                game.set_last_move_canidates(candidates);

                last_move = selected_move;

                std::string vertex = game.move_to_text(last_move);
                if (!analysis_output) {
                    gtp_printf(id, "%s", vertex.c_str());
                } else {
                    gtp_printf_raw("play %s\n", vertex.c_str());
                }
            }
            if (cfg_allow_pondering) {
                // now start pondering
                if (!game.has_resigned()) {
                    // Outputs winrate and pvs through gtp for lz-genmove_analyze
                    search->ponder();
                }
            }
            if (analysis_output) {
                // Terminate multi-line response
                gtp_printf_raw("\n");
            }
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        analysis_output = false;
        return;
    } else if (command.find("lz-analyze") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        auto who = game.board.get_to_move();

        cmdstream >> tmp; // eat lz-analyze
        cmdstream >> tmp; // eat side to move or interval
        if (!cmdstream.fail()) {
            if (tmp == "w" || tmp == "white") {
                who = FastBoard::WHITE;
            } else if (tmp == "b" || tmp == "black") {
                who = FastBoard::BLACK;
            } else {
                // Not side to move, must be interval
                try {
                    cfg_analyze_interval_centis = std::stoi(tmp);
                } catch(...) {
                    gtp_fail_printf(id, "syntax not understood");
                    return;
                }
            }
            if (tmp == "w" || tmp == "b" || tmp == "white" || tmp == "black") {
                // We got a color, so the interval must come now.
                int interval;
                cmdstream >> interval;
                if (!cmdstream.fail()) {
                    cfg_analyze_interval_centis = interval;
                } else {
                    gtp_fail_printf(id, "syntax not understood");
                    return;
                }
            }
        }
        // Start multi-line response.
        if (id != -1) gtp_printf_raw("=%d\n", id);
        else gtp_printf_raw("=\n");
        // Now start pondering.
        if (!game.has_resigned()) {
            // Outputs winrate and pvs through gtp
            game.set_to_move(who);
            search->ponder();
        }
        cfg_analyze_interval_centis = 0;
        // Terminate multi-line response
        gtp_printf_raw("\n");
        return;
    } else if (command.find("kgs-genmove_cleanup") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;

        cmdstream >> tmp;  // eat kgs-genmove
        cmdstream >> tmp;

        if (!cmdstream.fail()) {
            int who;
            if (tmp == "w" || tmp == "white") {
                who = FastBoard::WHITE;
            } else if (tmp == "b" || tmp == "black") {
                who = FastBoard::BLACK;
            } else {
                gtp_fail_printf(id, "syntax error");
                return;
            }
            game.set_passes(0);
            {
                game.set_to_move(who);
                int move = search->think(who, UCTSearch::NOPASS);
                game.play_move(move);

                std::string vertex = game.move_to_text(move);
                gtp_printf(id, "%s", vertex.c_str());
            }
            if (cfg_allow_pondering) {
                // now start pondering
                if (!game.has_resigned()) {
                    search->ponder();
                }
            }
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        return;
    } else if (command.find("undo") == 0) {
        if (game.undo_move()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "cannot undo");
        }
        return;
    } else if (command.find("showboard") == 0) {
        gtp_printf(id, "");
        game.display_state();
        return;
    } else if (command.find("final_score") == 0) {
        float ftmp = game.final_score();
        /* white wins */
        if (ftmp < -0.1) {
            gtp_printf(id, "W+%3.1f", float(fabs(ftmp)));
        } else if (ftmp > 0.1) {
            gtp_printf(id, "B+%3.1f", ftmp);
        } else {
            gtp_printf(id, "0");
        }
        return;
    } else if (command.find("final_status_list") == 0) {
        if (command.find("alive") != std::string::npos) {
            std::string livelist = get_life_list(game, true);
            gtp_printf(id, livelist.c_str());
        } else if (command.find("dead") != std::string::npos) {
            std::string deadlist = get_life_list(game, false);
            gtp_printf(id, deadlist.c_str());
        } else {
            gtp_printf(id, "");
        }
        return;
    } else if (command.find("time_settings") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        int maintime, byotime, byostones;

        cmdstream >> tmp >> maintime >> byotime >> byostones;

        if (!cmdstream.fail()) {
            // convert to centiseconds and set
            game.set_timecontrol(maintime * 100, byotime * 100, byostones, 0);

            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        return;
    } else if (command.find("time_left") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, color;
        int time, stones;

        cmdstream >> tmp >> color >> time >> stones;

        if (!cmdstream.fail()) {
            int icolor;

            if (color == "w" || color == "white") {
                icolor = FastBoard::WHITE;
            } else if (color == "b" || color == "black") {
                icolor = FastBoard::BLACK;
            } else {
                gtp_fail_printf(id, "Color in time adjust not understood.\n");
                return;
            }

            game.adjust_time(icolor, time * 100, stones);

            gtp_printf(id, "");

            if (cfg_allow_pondering) {
                // KGS sends this after our move
                // now start pondering
                if (!game.has_resigned()) {
                    search->ponder();
                }
            }
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        return;
    }else if(command.find("check_running") == 0){
        gtp_printf_raw("%s\n", search->is_running()?"True":"False");
        return;
    } else if(command.find("lastmove") == 0){

        int move = last_move;

        std::ostringstream result;

        int column = move % 15;
        int row = move / 15;

        column--;
        row--;

        assert(move == FastBoard::PASS
               || move == FastBoard::RESIGN
               || (row >= 0 && row < 13));
        assert(move == FastBoard::PASS
               || move == FastBoard::RESIGN
               || (column >= 0 && column < 13));

        if (move >= 0 && move <= 15*15) {

            if(column>=8){
                result << static_cast<char>('A' + column + 1);
            } else{
                result << static_cast<char>('A' + column);
            }
            result << (row + 1);
        } else if (move == FastBoard::PASS) {
            result << "pass";
        } else if (move == FastBoard::RESIGN) {
            result << "resign";
        } else {
            result << "error";
        }

        gtp_printf_raw("%s\n", result.str().c_str());

        return;
    }else if (command.find("autotrain") == 0) {
        int boardsize = game.board.get_boardsize();
        std::istringstream cmdstream(command);
        std::string tmp, filename;
        int train_count;

        cmdstream >> tmp >> filename >> train_count;

        auto chunker = OutputChunker{filename, true};

        std::random_device rd;
        std::ranlux48 gen(rd());

        for(int i=0; i<1; i++) {
            int movecount = 0;
            int winner = 0;
            int random_move = gen() % 60;
            search->set_playout_limit(gen() % 10 + 10);
            myprintf("random move for : %d\n", random_move);
            do {
                if(random_move == movecount) {
                    Training::clear_training();
                    search->set_playout_limit(cfg_max_playouts);
                }
                int move = search->think(game.get_to_move(), UCTSearch::NORMAL);
                game.play_move(move);
                game.display_state();

                movecount++;
                if (game.has_resigned()) {
                    winner = 1 - game.who_resigned();
                    break;
                }
                else if(movecount >= boardsize * boardsize *2) {
                    float ftmp = game.final_score();
                    if (ftmp < -0.1) {
                        winner = 1;
                    } else if (ftmp > 0.1) {
                        winner = 0;
                    } else {
                        // draw?
                        winner = -1;
                    }
                    break;
                }
                else if(game.get_passes() == 2) {
                    float ftmp = game.final_score();
                    if (ftmp < -0.1) {
                        winner = 1;
                    } else if (ftmp > 0.1) {
                        winner = 0;
                    } else {
                        // draw?
                        winner = -1;
                    }
                    break;
                }
            } while (true);


            myprintf("winner is : %s\n", winner ? "W" : "B");

            if(winner >= 0) {
                Training::dump_training(winner, chunker);
            }

            // re-init new game
            float old_komi = game.get_komi();
            Training::clear_training();
            game.init_game(13, old_komi);
        }
        return;
    } else if (command.find("auto") == 0) {
        do {
            int move = search->think(game.get_to_move(), UCTSearch::NORMAL);
            game.play_move(move);
            game.display_state();

        } while (game.get_passes() < 2 && !game.has_resigned());
        return;
    } else if (command.find("go") == 0) {
        int move = search->think(game.get_to_move());
        game.play_move(move);

        std::string vertex = game.move_to_text(move);
        myprintf("%s\n", vertex.c_str());
        return;
    } else if (command.find("heatmap") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        std::string symmetry;

        cmdstream >> tmp;   // eat heatmap
        cmdstream >> symmetry;

        Network::Netresult vec;
        if (cmdstream.fail()) {
            // Default = DIRECT with no symmetric change
            vec = s_network->get_output(
                    &game, Network::Ensemble::DIRECT,
                    Network::IDENTITY_SYMMETRY, true);
        } else if (symmetry == "all") {
            for (auto s = 0; s < Network::NUM_SYMMETRIES; ++s) {
                vec = s_network->get_output(
                        &game, Network::Ensemble::DIRECT, s, true);
                Network::show_heatmap(&game, vec, false);
            }
        } else if (symmetry == "average" || symmetry == "avg") {
            vec = s_network->get_output(
                    &game, Network::Ensemble::AVERAGE,
                    Network::NUM_SYMMETRIES, true);
        } else {
            vec = s_network->get_output(
                    &game, Network::Ensemble::DIRECT, std::stoi(symmetry), true);
        }

        if (symmetry != "all") {
            Network::show_heatmap(&game, vec, false);
        }

        gtp_printf(id, "");
        return;
    } else if (command.find("fixed_handicap") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        int stones;

        cmdstream >> tmp;   // eat fixed_handicap
        cmdstream >> stones;

        if (!cmdstream.fail() && game.set_fixed_handicap(stones)) {
            auto stonestring = game.board.get_stone_list();
            gtp_printf(id, "%s", stonestring.c_str());
        } else {
            gtp_fail_printf(id, "Not a valid number of handicap stones");
        }
        return;
    } else if (command.find("place_free_handicap") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        int stones;

        cmdstream >> tmp;   // eat place_free_handicap
        cmdstream >> stones;

        if (!cmdstream.fail()) {
            game.place_free_handicap(stones, *s_network);
            auto stonestring = game.board.get_stone_list();
            gtp_printf(id, "%s", stonestring.c_str());
        } else {
            gtp_fail_printf(id, "Not a valid number of handicap stones");
        }

        return;
    } else if (command.find("set_free_handicap") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;

        cmdstream >> tmp;   // eat set_free_handicap

        do {
            std::string vertex;

            cmdstream >> vertex;

            if (!cmdstream.fail()) {
                if (!game.play_textmove("black", vertex)) {
                    gtp_fail_printf(id, "illegal move");
                } else {
                    game.set_handicap(game.get_handicap() + 1);
                }
            }
        } while (!cmdstream.fail());

        std::string stonestring = game.board.get_stone_list();
        gtp_printf(id, "%s", stonestring.c_str());

        return;
    } else if (command.find("loadsgf") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, filename;
        int movenum;

        cmdstream >> tmp;   // eat loadsgf
        cmdstream >> filename;

        if (!cmdstream.fail()) {
            cmdstream >> movenum;

            if (cmdstream.fail()) {
                movenum = 999;
            }
        } else {
            gtp_fail_printf(id, "Missing filename.");
            return;
        }

        auto sgftree = std::make_unique<SGFTree>();

        try {
            sgftree->load_from_file(filename);
            game = sgftree->follow_mainline_state(movenum - 1);
            gtp_printf(id, "");
        } catch (const std::exception&) {
            gtp_fail_printf(id, "cannot load file");
        }
        return;
    } else if (command.find("kgs-chat") == 0) {
        // kgs-chat (game|private) Name Message
        std::istringstream cmdstream(command);
        std::string tmp;

        cmdstream >> tmp; // eat kgs-chat
        cmdstream >> tmp; // eat game|private
        cmdstream >> tmp; // eat player name
        do {
            cmdstream >> tmp; // eat message
        } while (!cmdstream.fail());

        gtp_fail_printf(id, "I'm a go bot, not a chat bot.");
        return;
    } else if (command.find("kgs-game_over") == 0) {
        // Do nothing. Particularly, don't ponder.
        gtp_printf(id, "");
        return;
    } else if (command.find("kgs-time_settings") == 0) {
        // none, absolute, byoyomi, or canadian
        std::istringstream cmdstream(command);
        std::string tmp;
        std::string tc_type;
        int maintime, byotime, byostones, byoperiods;

        cmdstream >> tmp >> tc_type;

        if (tc_type.find("none") != std::string::npos) {
            // 30 mins
            game.set_timecontrol(30 * 60 * 100, 0, 0, 0);
        } else if (tc_type.find("absolute") != std::string::npos) {
            cmdstream >> maintime;
            game.set_timecontrol(maintime * 100, 0, 0, 0);
        } else if (tc_type.find("canadian") != std::string::npos) {
            cmdstream >> maintime >> byotime >> byostones;
            // convert to centiseconds and set
            game.set_timecontrol(maintime * 100, byotime * 100, byostones, 0);
        } else if (tc_type.find("byoyomi") != std::string::npos) {
            // KGS style Fischer clock
            cmdstream >> maintime >> byotime >> byoperiods;
            game.set_timecontrol(maintime * 100, byotime * 100, 0, byoperiods);
        } else {
            gtp_fail_printf(id, "syntax not understood");
            return;
        }

        if (!cmdstream.fail()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        return;
    } else if (command.find("netbench") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp;
        int iterations;

        cmdstream >> tmp;  // eat netbench
        cmdstream >> iterations;

        if (!cmdstream.fail()) {
            s_network->benchmark(&game, iterations);
        } else {
            s_network->benchmark(&game);
        }
        gtp_printf(id, "");
        return;

    } else if (command.find("printsgf") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, filename;

        cmdstream >> tmp;   // eat printsgf
        cmdstream >> filename;

        auto sgf_text = SGFTree::state_to_string(game, 0);
        // GTP says consecutive newlines terminate the output,
        // so we must filter those.
        boost::replace_all(sgf_text, "\n\n", "\n");

        if (cmdstream.fail()) {
            gtp_printf(id, "%s\n", sgf_text.c_str());
        } else {
            std::ofstream out(filename);
            out << sgf_text;
            out.close();
            gtp_printf(id, "");
        }

        return;
    } else if (command.find("load_training") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, filename;

        // tmp will eat "load_training"
        cmdstream >> tmp >> filename;

        Training::load_training(filename);

        if (!cmdstream.fail()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }

        return;
    } else if (command.find("save_training") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, filename;

        // tmp will eat "save_training"
        cmdstream >> tmp >>  filename;

        Training::save_training(filename);

        if (!cmdstream.fail()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }

        return;
    } else if (command.find("dump_training") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, winner_color, filename;
        int who_won;

        // tmp will eat "dump_training"
        cmdstream >> tmp >> winner_color >> filename;

        if (winner_color == "w" || winner_color == "white") {
            who_won = FullBoard::WHITE;
        } else if (winner_color == "b" || winner_color == "black") {
            who_won = FullBoard::BLACK;
        } else {
            gtp_fail_printf(id, "syntax not understood");
            return;
        }

        Training::dump_training(who_won, filename);

        if (!cmdstream.fail()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }

        return;
    } else if (command.find("dump_debug") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, filename;

        // tmp will eat "dump_debug"
        cmdstream >> tmp >> filename;

        Training::dump_debug(filename);

        if (!cmdstream.fail()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }

        return;
    } else if (command.find("dump_supervised") == 0) {
        std::istringstream cmdstream(command);
        std::string tmp, sgfname, outname;

        // tmp will eat dump_supervised
        cmdstream >> tmp >> sgfname >> outname;

        Training::dump_supervised(sgfname, outname);

        if (!cmdstream.fail()) {
            gtp_printf(id, "");
        } else {
            gtp_fail_printf(id, "syntax not understood");
        }
        return;
    } else if (command.find("lz-memory_report") == 0) {
        auto base_memory = get_base_memory();
        auto tree_size = add_overhead(UCTNodePointer::get_tree_size());
        auto cache_size = add_overhead(s_network->get_estimated_cache_size());

        auto total = base_memory + tree_size + cache_size;
        gtp_printf(id,
                   "Estimated total memory consumption: %d MiB.\n"
                   "Network with overhead: %d MiB / Search tree: %d MiB / Network cache: %d\n",
                   total / MiB, base_memory / MiB, tree_size / MiB, cache_size / MiB);
        return;
    } else if (command.find("lz-setoption") == 0) {
        return execute_setoption(*search.get(), id, command);
    }
    gtp_fail_printf(id, "unknown command");
    return;
}

std::pair<std::string, std::string> GTP::parse_option(std::istringstream& is) {
    std::string token, name, value;

    // Read option name (can contain spaces)
    while (is >> token && token != "value")
        name += std::string(" ", name.empty() ? 0 : 1) + token;

    // Read option value (can contain spaces)
    while (is >> token)
        value += std::string(" ", value.empty() ? 0 : 1) + token;

    return std::make_pair(name, value);
}

size_t GTP::get_base_memory() {
    // At the moment of writing the memory consumption is
    // roughly network size + 85 for one GPU and + 160 for two GPUs.
#ifdef USE_OPENCL
    auto gpus = std::max(cfg_gpus.size(), size_t{1});
    return s_network->get_estimated_size() + 85 * MiB * gpus;
#else
    return s_network->get_estimated_size();
#endif
}

std::pair<bool, std::string> GTP::set_max_memory(size_t max_memory,
                                                 int cache_size_ratio_percent) {
    if (max_memory == 0) {
        max_memory = UCTSearch::DEFAULT_MAX_MEMORY;
    }

    // Calculate amount of memory available for the search tree +
    // NNCache by estimating a constant memory overhead first.
    auto base_memory = get_base_memory();

    if (max_memory < base_memory) {
        return std::make_pair(false, "Not enough memory for network. " +
                                     std::to_string(base_memory / MiB) + " MiB required.");
    }

    auto max_memory_for_search = max_memory - base_memory;

    assert(cache_size_ratio_percent >= 1);
    assert(cache_size_ratio_percent <= 99);
    auto max_cache_size = max_memory_for_search *
                          cache_size_ratio_percent / 100;

    auto max_cache_count =
            (int)(remove_overhead(max_cache_size) / NNCache::ENTRY_SIZE);

    // Verify if the setting would not result in too little cache.
    if (max_cache_count < NNCache::MIN_CACHE_COUNT) {
        return std::make_pair(false, "Not enough memory for cache.");
    }
    auto max_tree_size = max_memory_for_search - max_cache_size;

    if (max_tree_size < UCTSearch::MIN_TREE_SPACE) {
        return std::make_pair(false, "Not enough memory for search tree.");
    }

    // Only if settings are ok we store the values in config.
    cfg_max_memory = max_memory;
    cfg_max_cache_ratio_percent = cache_size_ratio_percent;
    // Set max_tree_size.
    cfg_max_tree_size = remove_overhead(max_tree_size);
    // Resize cache.
    s_network->nncache_resize(max_cache_count);

    return std::make_pair(true, "Setting max tree size to " +
                                std::to_string(max_tree_size / MiB) + " MiB and cache size to " +
                                std::to_string(max_cache_size / MiB) +
                                " MiB.");
}

void GTP::execute_setoption(UCTSearch & search,
                            int id, const std::string &command) {
    std::istringstream cmdstream(command);
    std::string tmp, name_token;

    // Consume lz_setoption, name.
    cmdstream >> tmp >> name_token;

    // Print available options if called without an argument.
    if (cmdstream.fail()) {
        std::string options_out_tmp("");
        for (int i = 0; s_options[i].size() > 0; i++) {
            options_out_tmp = options_out_tmp + "\n" + s_options[i];
        }
        gtp_printf(id, options_out_tmp.c_str());
        return;
    }

    if (name_token.find("name") != 0) {
        gtp_fail_printf(id, "incorrect syntax for lz-setoption");
        return;
    }

    std::string name, value;
    std::tie(name, value) = parse_option(cmdstream);

    if (name == "maximum memory use (mib)") {
        std::istringstream valuestream(value);
        int max_memory_in_mib;
        valuestream >> max_memory_in_mib;
        if (!valuestream.fail()) {
            if (max_memory_in_mib < 128 || max_memory_in_mib > 131072) {
                gtp_fail_printf(id, "incorrect value");
                return;
            }
            bool result;
            std::string reason;
            std::tie(result, reason) = set_max_memory(max_memory_in_mib * MiB,
                                                      cfg_max_cache_ratio_percent);
            if (result) {
                gtp_printf(id, reason.c_str());
            } else {
                gtp_fail_printf(id, reason.c_str());
            }
            return;
        } else {
            gtp_fail_printf(id, "incorrect value");
            return;
        }
    } else if (name == "percentage of memory for cache") {
        std::istringstream valuestream(value);
        int cache_size_ratio_percent;
        valuestream >> cache_size_ratio_percent;
        if (cache_size_ratio_percent < 1 || cache_size_ratio_percent > 99) {
            gtp_fail_printf(id, "incorrect value");
            return;
        }
        bool result;
        std::string reason;
        std::tie(result, reason) = set_max_memory(cfg_max_memory,
                                                  cache_size_ratio_percent);
        if (result) {
            gtp_printf(id, reason.c_str());
        } else {
            gtp_fail_printf(id, reason.c_str());
        }
        return;
    } else if (name == "visits") {
        std::istringstream valuestream(value);
        int visits;
        valuestream >> visits;
        cfg_max_visits = visits;

        // 0 may be specified to mean "no limit"
        if (cfg_max_visits == 0) {
            cfg_max_visits = UCTSearch::UNLIMITED_PLAYOUTS;
        }
        // Note that if the visits are changed but no
        // explicit command to set memory usage is given,
        // we will stick with the initial guess we made on startup.
        search.set_visit_limit(cfg_max_visits);

        gtp_printf(id, "");
    } else if (name == "playouts") {
        std::istringstream valuestream(value);
        int playouts;
        valuestream >> playouts;
        cfg_max_playouts = playouts;

        // 0 may be specified to mean "no limit"
        if (cfg_max_playouts == 0) {
            cfg_max_playouts = UCTSearch::UNLIMITED_PLAYOUTS;
        } else if (cfg_allow_pondering) {
            // Limiting playouts while pondering is still enabled
            // makes no sense.
            gtp_fail_printf(id, "incorrect value");
            return;
        }

        // Note that if the playouts are changed but no
        // explicit command to set memory usage is given,
        // we will stick with the initial guess we made on startup.
        search.set_playout_limit(cfg_max_visits);

        gtp_printf(id, "");
    } else if (name == "lagbuffer") {
        std::istringstream valuestream(value);
        int lagbuffer;
        valuestream >> lagbuffer;
        cfg_lagbuffer_cs = lagbuffer;
        gtp_printf(id, "");
    } else if (name == "pondering") {
        std::istringstream valuestream(value);
        std::string toggle;
        valuestream >> toggle;
        if (toggle == "true") {
            if (cfg_max_playouts != UCTSearch::UNLIMITED_PLAYOUTS) {
                gtp_fail_printf(id, "incorrect value");
                return;
            }
            cfg_allow_pondering = true;
        } else if (toggle == "false") {
            cfg_allow_pondering = false;
        } else {
            gtp_fail_printf(id, "incorrect value");
            return;
        }
        gtp_printf(id, "");
    } else if (name == "resign percentage") {
        std::istringstream valuestream(value);
        int resignpct;
        valuestream >> resignpct;
        cfg_resignpct = resignpct;
        gtp_printf(id, "");
    } else {
        gtp_fail_printf(id, "Unknown option");
    }
    return;
}
