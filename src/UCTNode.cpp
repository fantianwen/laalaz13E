/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2018 Gian-Carlo Pascutto

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

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>
#include <iostream>
#include <sstream>

#include "UCTNode.h"
#include "FastBoard.h"
#include "FastState.h"
#include "GTP.h"
#include "GameState.h"
#include "Network.h"
#include "Utils.h"

using namespace Utils;

UCTNode::UCTNode(int vertex, float policy) : m_move(vertex), m_policy(policy) {
}

bool UCTNode::first_visit() const {
    return m_visits == 0;
}


void UCTNode::get_static_policy(Network & network,
                                GameState& state){

    const auto raw_netlist = network.get_output(
            &state, Network::Ensemble::RANDOM_SYMMETRY);

    const auto to_move = state.board.get_to_move();

    std::vector<Network::PolicyVertexPair> nodelist;

    auto legal_sum = 0.0f;
    for (auto i = 0; i < NUM_INTERSECTIONS; i++) {
        const auto x = i % BOARD_SIZE;
        const auto y = i / BOARD_SIZE;
        const auto vertex = state.board.get_vertex(x, y);
        if (state.is_move_legal(to_move, vertex)) {
            nodelist.emplace_back(raw_netlist.policy[i], vertex);
            legal_sum += raw_netlist.policy[i];
        }
    }
    nodelist.emplace_back(raw_netlist.policy_pass, FastBoard::PASS);
    legal_sum += raw_netlist.policy_pass;

    if (legal_sum > std::numeric_limits<float>::min()) {
        // re-normalize after removing illegal moves.
        for (auto& node : nodelist) {
            node.first /= legal_sum;
        }
    } else {
        // This can happen with new randomized nets.
        auto uniform_prob = 1.0f / nodelist.size();
        for (auto& node : nodelist) {
            node.first = uniform_prob;
        }
    }

    this->initial_node_list = nodelist;
    printf("git static policy from network! \n");

}

bool UCTNode::create_children(Network & network,
                              std::atomic<int>& nodecount,
                              GameState& state,
                              float& eval,
                              float min_psa_ratio) {
    // no successors in final state
    if (state.get_passes() >= 2) {
        return false;
    }

    // acquire the lock
    if (!acquire_expanding()) {
        return false;
    }

    // can we actually expand?
    if (!expandable(min_psa_ratio)) {
        expand_done();
        return false;
    }

    const auto raw_netlist = network.get_output(
        &state, Network::Ensemble::RANDOM_SYMMETRY);

    // DCNN returns winrate as side to move
    m_net_eval = raw_netlist.winrate;
    const auto to_move = state.board.get_to_move();
    // our search functions evaluate from black's point of view
    if (state.board.white_to_move()) {
        m_net_eval = 1.0f - m_net_eval;
    }
    eval = m_net_eval;

    std::vector<Network::PolicyVertexPair> nodelist;

    auto legal_sum = 0.0f;
    for (auto i = 0; i < NUM_INTERSECTIONS; i++) {
        const auto x = i % BOARD_SIZE;
        const auto y = i / BOARD_SIZE;
        const auto vertex = state.board.get_vertex(x, y);
        if (state.is_move_legal(to_move, vertex)) {
            nodelist.emplace_back(raw_netlist.policy[i], vertex);
            legal_sum += raw_netlist.policy[i];
        }
    }
    nodelist.emplace_back(raw_netlist.policy_pass, FastBoard::PASS);
    legal_sum += raw_netlist.policy_pass;

    if (legal_sum > std::numeric_limits<float>::min()) {
        // re-normalize after removing illegal moves.
        for (auto& node : nodelist) {
            node.first /= legal_sum;
        }
    } else {
        // This can happen with new randomized nets.
        auto uniform_prob = 1.0f / nodelist.size();
        for (auto& node : nodelist) {
            node.first = uniform_prob;
        }
    }

    link_nodelist(nodecount, nodelist, min_psa_ratio);

    expand_done();
    return true;
}

void UCTNode::link_nodelist(std::atomic<int>& nodecount,
                            std::vector<Network::PolicyVertexPair>& nodelist,
                            float min_psa_ratio) {
    assert(min_psa_ratio < m_min_psa_ratio_children);

    if (nodelist.empty()) {
        return;
    }

    // Use best to worst order, so highest go first
    std::stable_sort(rbegin(nodelist), rend(nodelist));

    const auto max_psa = nodelist[0].first;
    const auto old_min_psa = max_psa * m_min_psa_ratio_children;
    const auto new_min_psa = max_psa * min_psa_ratio;
    if (new_min_psa > 0.0f) {
        m_children.reserve(
            std::count_if(cbegin(nodelist), cend(nodelist),
                [=](const auto& node) { return node.first >= new_min_psa; }
            )
        );
    } else {
        m_children.reserve(nodelist.size());
    }

    auto skipped_children = false;
    for (const auto& node : nodelist) {
        if (node.first < new_min_psa) {
            skipped_children = true;
        } else if (node.first < old_min_psa) {
            m_children.emplace_back(node.second, node.first);
            ++nodecount;
        }
    }

    m_min_psa_ratio_children = skipped_children ? min_psa_ratio : 0.0f;
}

std::vector<UCTNodePointer>& UCTNode::get_children() {
    return m_children;
}


float UCTNode::get_static_sp() const {
    return m_static_sp;
}

int UCTNode::get_move() const {
    return m_move;
}

void UCTNode::virtual_loss() {
    m_virtual_loss += VIRTUAL_LOSS_COUNT;
}

void UCTNode::virtual_loss_undo() {
    m_virtual_loss -= VIRTUAL_LOSS_COUNT;
}

void UCTNode::update(float eval) {
    m_visits++;
    accumulate_eval(eval);
}

bool UCTNode::has_children() const {
    return m_min_psa_ratio_children <= 1.0f;
}

bool UCTNode::expandable(const float min_psa_ratio) const {
#ifndef NDEBUG
    if (m_min_psa_ratio_children == 0.0f) {
        // If we figured out that we are fully expandable
        // it is impossible that we stay in INITIAL state.
        assert(m_expand_state.load() != ExpandState::INITIAL);
    }
#endif
    return min_psa_ratio < m_min_psa_ratio_children;
}

float UCTNode::get_policy() const {
    return m_policy;
}

void UCTNode::set_policy(float policy) {
    m_policy = policy;
}

int UCTNode::get_visits() const {
    return m_visits;
}

float UCTNode::get_raw_eval(int tomove, int virtual_loss) const {
    auto visits = get_visits() + virtual_loss;
    assert(visits > 0);
    auto blackeval = get_blackevals();
    if (tomove == FastBoard::WHITE) {
        blackeval += static_cast<double>(virtual_loss);
    }
    auto eval = static_cast<float>(blackeval / double(visits));
    if (tomove == FastBoard::WHITE) {
        eval = 1.0f - eval;
    }
    return eval;
}

float UCTNode::get_eval(int tomove) const {
    // Due to the use of atomic updates and virtual losses, it is
    // possible for the visit count to change underneath us. Make sure
    // to return a consistent result to the caller by caching the values.
    return get_raw_eval(tomove, m_virtual_loss);
}

float UCTNode::get_net_eval(int tomove) const {
    if (tomove == FastBoard::WHITE) {
        return 1.0f - m_net_eval;
    }
    return m_net_eval;
}

double UCTNode::get_blackevals() const {
    return m_blackevals;
}

void UCTNode::accumulate_eval(float eval) {
    atomic_add(m_blackevals, double(eval));
}

UCTNode* UCTNode::uct_select_child(int color, bool is_root) {
    wait_expanded();

    // Count parentvisits manually to avoid issues with transpositions.
    auto total_visited_policy = 0.0f;
    auto parentvisits =size_t{0};
    for (const auto& child : m_children) {
        if (child.valid()) {
            parentvisits += child.get_visits();
            if (child.get_visits() > 0) {
                total_visited_policy += child.get_policy();
            }
        }
    }

    const auto numerator = std::sqrt(double(parentvisits));
    // todo how it works
    const auto fpu_reduction = (is_root ? cfg_fpu_root_reduction : cfg_fpu_reduction) * std::sqrt(total_visited_policy);
    // Estimated eval forknown nodes = original parent NN eval - reduction
    const auto fpu_eval = get_net_eval(color) - fpu_reduction;

    auto best = static_cast<UCTNodePointer*>(nullptr);
    auto best_value = std::numeric_limits<double>::lowest();

    for (auto& child : m_children) {
        if (!child.active()) {
            continue;
        }

        auto winrate = fpu_eval;
        if (child.is_inflated() && child->m_expand_state.load() == ExpandState::EXPANDING) {
            // Someone else is expanding this node, never select it
            // if we can avoid so, because we'd block on it.
            winrate = -1.0f - fpu_reduction;
        } else if (child.get_visits() > 0) {
            winrate = child.get_eval(color);
        }
        const auto psa = child.get_policy();
        const auto denom = 1.0 + child.get_visits();
        const auto puct = cfg_puct * psa * (numerator / denom);
        const auto value = winrate + puct;
        assert(value > std::numeric_limits<double>::lowest());

        if (value > best_value) {
            best_value = value;
            best = &child;
        }
    }

    assert(best != nullptr);
    best->inflate();
    return best->get();
}

class NodeComp : public std::binary_function<UCTNodePointer&,
                                             UCTNodePointer&, bool> {
public:
    NodeComp(int color) : m_color(color) {};
    bool operator()(const UCTNodePointer& a,
                    const UCTNodePointer& b) {
//         if visits are not same, sort on visits
        if (a.get_visits() != b.get_visits()) {
            return a.get_visits() < b.get_visits();
        }

        // neither has visits, sort on policy prior
        if (a.get_visits() == 0) {
            return a.get_policy() < b.get_policy();
        }

//         both have same non-zero number of visits
        return a.get_eval(m_color) < b.get_eval(m_color);

//        if(a.get_visits()>0 && b.get_visits()>0){
//            return a.get_visits() < b.get_visits();
////            return a.get_eval(m_color)<b.get_eval(m_color);
//        }
//
//        return a.get_policy() < b.get_policy();
    }
private:
    int m_color;
};


class NodeComp2 : public std::binary_function<UCTNodePointer&,
        UCTNodePointer&, bool> {
public:
    bool operator()(const UCTNodePointer& a,
                    const UCTNodePointer& b) {
        return a.get_move() > b.get_move();
    }
};

void UCTNode::sort_children(int color) {
    std::stable_sort(rbegin(m_children), rend(m_children), NodeComp(color));
}


std::string UCTNode::transforMoveForSGF(int move) const{

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

    // SGF inverts rows
    row = 13 - row - 1;

    if (move >= 0 && move <= 15*15) {
        if (column <= 25) {
            result << static_cast<char>('a' + column);
        } else {
            result << static_cast<char>('A' + column - 26);
        }
        if (row <= 25) {
            result << static_cast<char>('a' + row);
        } else {
            result << static_cast<char>('A' + row - 26);
        }
    } else if (move == FastBoard::PASS) {
        result << "tt";
    } else if (move == FastBoard::RESIGN) {
        result << "tt";
    } else {
        result << "error";
    }

    return result.str();
}

std::string UCTNode::transferMove(int move) const {
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
        result << static_cast<char>(column < 8 ? 'A' + column : 'A' + column + 1);
        result << (row + 1);
    } else if (move == FastBoard::PASS) {
        result << "pass";
    } else if (move == FastBoard::RESIGN) {
        result << "resign";
    } else {
        result << "error";
    }

    return result.str();
}

std::string UCTNode::print_candidates(int color,float selectedWinrate){

    printf("begin to show candidates moves \n");

    std::string candidatesString = "";

    int index = 0;
//
//    for (const auto& child : get_children()){
//        index++;
//        if(child->get_visits()>0) {
//            auto move = child->get_move();
//            candidatesString += "["+transforMoveForSGF(move)+":"+std::to_string(index)+"]";
//        }
//    }

    index = 0;

    candidatesString+=std::to_string(selectedWinrate)+"::\n";

    candidatesString+="index\tvertex\twr\tvisit\tsp\ts_sp\n";

    for (const auto& child : get_children()) {
        index++;
        if(child->get_visits()>0) {
            int visitCount = child->get_visits();
            auto move_policy = static_cast<float>(((float)visitCount / get_visits()));
            auto prob = child.get_eval(color);
            auto move = child->get_move();
            auto s_sp = child->get_static_sp();

            candidatesString += std::to_string(index)+"\t"+" "+" "+
                    transferMove(move)+"\t"+" "+" "+
                    std::to_string(prob)+"\t"+" "+" "+
                    std::to_string(visitCount)+"\t"+" "+" "+
                    std::to_string(move_policy)+"\t"+" "+" "+
                    std::to_string(s_sp)+"\n";
        }
    }

    printf("%s,",candidatesString.c_str());

    printf("show end!");

    return candidatesString;
}

void UCTNode::usingStrengthControl(int color,int lastMove){

            //case 1: the winrate dif between first and second move is too high(10%),we just use the first move;
    //case 2: the winrate of first is too low, we just select the first move;
    //case 3: intermedidate winrate (choose the move having the biggest winning rate within a T_dif )
    //case 4: hign win rate: for fast decrease of the winrate, we just select the worst move
        // (1) select move with the best select policy
        // (2) play the best move

    printf("using strength control \n");

    int index = 0;
    case_three = false;

    float first = 0,second = 0;


    for (const auto& child : get_children()) {

        if(index==0){
            first = child.get_eval(color);
        }

        if(index==1){
            second = child.get_eval(color);
        }

        index ++;

        for (const auto& initial_node: this->initial_node_list){
            if(initial_node.second==child.get_move()){
                child->m_static_sp = initial_node.first;
            }
        }

    }

    printf("the first wr: %f, the second wr: %f",first,second);

    if(accord_case_one(first,second)){
        // do nothing
        printf("accord with case one \n");
    }else if(accord_case_two(first)){
        //do nothing
        printf("accord with case two \n");
    }else if(first>=t_min && first<=t_max){
        // do nothing

        accord_case_three(color,first-t_dif);
        printf("accord with case three \n");
        printf("case three move is %d \n",case_three_move);

    }else{
        accord_case_three_one(color,lastMove);
    }

}

bool UCTNode::accord_case_one(float first,float second){
    return first-second>=t_uniq;
}

bool UCTNode::accord_case_two(float first){
    return first<=t_min;
}

bool UCTNode::accord_case_three(int color,float threshold){

    case_three = true;

    float _sp = 0;

    for (const auto& child : get_children()) {

        if (child.get_eval(color)>=threshold) {
            if (child.get_static_sp() > _sp) {
                _sp = child.get_static_sp();
                case_three_move = child.get_move();
                case_three_winrate = child.get_eval(color);
            }
        }
    }

    return false;
}

bool UCTNode::accord_case_three_one(int color,int lastmove){

    float firstMoveRate;
    float allowedProb1,allowedProb2,allowedProb3,allowedProb4;
    float allowedPolicy1 = 0.05,allowedPolicy2=0.10,allowedPolicy3=0.20,allowedPolicy4=0.40;

    int _move = 0;

    firstMoveRate = get_first_child()->get_eval(color);

    allowedProb1 = firstMoveRate-(float)0.03*c_param;
    allowedProb2 = firstMoveRate-(float)0.04*c_param;
    allowedProb3 = firstMoveRate-(float)0.06*c_param;
    allowedProb4 = firstMoveRate-(float)0.08*c_param;

    case_three_move = get_first_child()->get_move();
    case_three_winrate = get_first_child()->get_eval(color);
    float _evaluation_rate = 0 ;

    for (const auto& child : get_children()) {
        int _visit = child->get_visits();
        _move = child->get_move();
        float policy = child.get_static_sp();
        auto move_policy = static_cast<float>(((float)child.get_visits() / get_visits()));

        auto prob = child.get_eval(color);

        if(_visit>=10) {

            if (prob >= allowedProb4 && prob <= allowedProb3 && policy >= allowedPolicy4) {

                printf("accord with case 3-4 \n");

                printf("policy is: %f,allowedPolicy is:%f.", policy, allowedPolicy4);

                case_three = true;

                //
                //            float dis = calulate_dis_between_moves(lastmove,_move);
                //            float evaluation_rate = (1-dis)*policy;
                //
                //            if (evaluation_rate>_evaluation_rate){
                //                case_three_move =_move;
                //                case_three_winrate = prob;
                //                _evaluation_rate = evaluation_rate;
                //            }

                if (case_three_winrate > prob) {
                    case_three_move = _move;
                    case_three_winrate = prob;
                }

            }

            if (prob >= allowedProb3 && prob <= allowedProb2 && policy >= allowedPolicy3) {

                printf("accord with case 3-3 \n");

                printf("policy is: %f,allowedPolicy is:%f.", policy, allowedPolicy3);

                case_three = true;

                //            float dis = calulate_dis_between_moves(lastmove,_move);
                //            float evaluation_rate = (1-dis)*policy;
                //            if (evaluation_rate>_evaluation_rate){
                //                case_three_move =_move;
                //                case_three_winrate = prob;
                //                _evaluation_rate = evaluation_rate;
                //            }

                if (case_three_winrate > prob) {
                    case_three_move = _move;
                    case_three_winrate = prob;
                }
            }


            if (prob >= allowedProb2 && prob <= allowedProb1 && policy >= allowedPolicy2) {

                printf("accord with case 3-2 \n");

                printf("policy is: %f,allowedPolicy is:%f.", policy, allowedPolicy2);

                case_three = true;

                //            float dis = calulate_dis_between_moves(lastmove,_move);
                //            float evaluation_rate = (1-dis)*policy;
                //            if (evaluation_rate>_evaluation_rate){
                //                case_three_move =_move;
                //                case_three_winrate = prob;
                //                _evaluation_rate = evaluation_rate;
                //            }

                if (case_three_winrate > prob) {
                    case_three_move = _move;
                    case_three_winrate = prob;
                }
            }

            if (prob >= allowedProb1 && policy > allowedPolicy1) {

                printf("accord with case 3-1 \n");

                printf("policy is: %f,allowedPolicy is:%f.", policy, allowedPolicy1);

                case_three = true;

                //            float dis = calulate_dis_between_moves(lastmove,_move);
                //            float evaluation_rate = (1-dis)*policy;
                //            if (evaluation_rate>_evaluation_rate){
                //                case_three_move =_move;
                //                case_three_winrate = prob;
                //                _evaluation_rate = evaluation_rate;
                //            }

                if (case_three_winrate > prob) {
                    case_three_move = _move;
                    case_three_winrate = prob;
                }

            }
        }
    }
    return false;
}

bool UCTNode::get_case_three_flag(){
    return case_three;
}

int UCTNode::get_case_three_move(){
    return case_three_move;
}

float UCTNode::get_case_three_winrate(){
    return case_three_winrate;
}


UCTNode& UCTNode::get_best_root_child(int color) {
    wait_expanded();

    assert(!m_children.empty());

    auto ret = std::max_element(begin(m_children), end(m_children),
                                NodeComp(color));
    ret->inflate();

    return *(ret->get());
}

size_t UCTNode::count_nodes_and_clear_expand_state() {
    auto nodecount = size_t{0};
    nodecount += m_children.size();
    if (expandable()) {
        m_expand_state = ExpandState::INITIAL;
    }
    for (auto& child : m_children) {
        if (child.is_inflated()) {
            nodecount += child->count_nodes_and_clear_expand_state();
        }
    }
    return nodecount;
}

void UCTNode::invalidate() {
    m_status = INVALID;
}

void UCTNode::set_active(const bool active) {
    if (valid()) {
        m_status = active ? ACTIVE : PRUNED;
    }
}

bool UCTNode::valid() const {
    return m_status != INVALID;
}

bool UCTNode::active() const {
    return m_status == ACTIVE;
}

bool UCTNode::acquire_expanding() {
    auto expected = ExpandState::INITIAL;
    auto newval = ExpandState::EXPANDING;
    return m_expand_state.compare_exchange_strong(expected, newval);
}

void UCTNode::expand_done() {
    auto v = m_expand_state.exchange(ExpandState::EXPANDED);
#ifdef NDEBUG
    (void)v;
#endif
    assert(v == ExpandState::EXPANDING);
}
void UCTNode::expand_cancel() {
    auto v = m_expand_state.exchange(ExpandState::INITIAL);
#ifdef NDEBUG
    (void)v;
#endif
    assert(v == ExpandState::EXPANDING);
}
void UCTNode::wait_expanded() {
    while (m_expand_state.load() == ExpandState::EXPANDING) {}
    auto v = m_expand_state.load();
#ifdef NDEBUG
    (void)v;
#endif
    assert(v == ExpandState::EXPANDED);
}

