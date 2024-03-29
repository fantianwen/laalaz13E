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

#ifndef UCTNODE_H_INCLUDED
#define UCTNODE_H_INCLUDED

#include "config.h"

#include <atomic>
#include <memory>
#include <vector>
#include <cassert>
#include <cstring>

#include "GameState.h"
#include "Network.h"
#include "SMP.h"
#include "UCTNodePointer.h"

class UCTNode {
public:
    // When we visit a node, add this amount of virtual losses
    // to it to encourage other CPUs to explore other parts of the
    // search tree.
    static constexpr auto VIRTUAL_LOSS_COUNT = 3;
    // Defined in UCTNode.cpp
    explicit UCTNode(int vertex, float policy);
    UCTNode() = delete;
    ~UCTNode() = default;

    void get_static_policy(Network & network,GameState& state);
    bool create_children(Network & network,
                         std::atomic<int>& nodecount,
                         GameState& state, float& eval,
                         float min_psa_ratio = 0.0f);

    std::vector<UCTNodePointer>& get_children();
    void sort_children(int color);
    std::string transforMoveForSGF(int move) const;
    std::string transferMove(int move) const;
    std::string print_candidates(int color,float selectedWinrate);
    void usingStrengthControl(int color,int lastmove);
    bool accord_case_one(float first,float second);
    bool accord_case_two(float first);
    bool accord_case_three(int color,float _dif);
    bool accord_case_three_one(int color,int lastmove);
    bool get_case_three_flag();
    int get_case_three_move();
    float get_case_three_winrate();
    UCTNode& get_best_root_child(int color);
    UCTNode* uct_select_child(int color, bool is_root);

    size_t count_nodes_and_clear_expand_state();
    bool first_visit() const;
    bool has_children() const;
    bool expandable(const float min_psa_ratio = 0.0f) const;
    const float c_param = 0.8;
    float t_dif = 0.03*c_param;
    float t_max = 0.60;
    float t_min = 0.40;
    float t_uniq = 0.08*c_param;// the gap
    void invalidate();
    void set_active(const bool active);
    bool valid() const;
    bool active() const;
    float get_static_sp() const ;
    int get_move() const;
    int get_visits() const;
    float get_policy() const;
    void set_policy(float policy);
    float get_eval(int tomove) const;
    float get_raw_eval(int tomove, int virtual_loss = 0) const;
    float get_net_eval(int tomove) const;
    void virtual_loss();
    void virtual_loss_undo();
    void update(float eval);

    // Defined in UCTNodeRoot.cpp, only to be called on m_root in UCTSearch
    void randomize_first_proportionally();
    void prepare_root_node(Network & network, int color,
                           std::atomic<int>& nodecount,
                           GameState& state);

    UCTNode* get_first_child() const;
    float calulate_dis_between_moves(int move1,int move2) const ;
    UCTNode* get_nopass_child(FastState& state) const;
    std::unique_ptr<UCTNode> find_child(const int move);
    void inflate_all_children();

    void clear_expand_state();
private:
    enum Status : char {
        INVALID, // superko
        PRUNED,
        ACTIVE
    };
    void link_nodelist(std::atomic<int>& nodecount,
                       std::vector<Network::PolicyVertexPair>& nodelist,
                       float min_psa_ratio);
    double get_blackevals() const;
    void accumulate_eval(float eval);
    void kill_superkos(const KoState& state);
    void dirichlet_noise(float epsilon, float alpha);

    // Note : This class is very size-sensitive as we are going to create
    // tens of millions of instances of these.  Please put extra caution
    // if you want to add/remove/reorder any variables here.

    // Move
    std::int16_t m_move;
    std::float_t m_static_sp{0.0f};
    // UCT
    std::atomic<std::int16_t> m_virtual_loss{0};
    std::atomic<int> m_visits{0};
    // UCT eval
    float m_policy;
    // Original net eval for this node (not children).
    float m_net_eval{0.0f};
    std::atomic<double> m_blackevals{0.0};
    std::atomic<Status> m_status{ACTIVE};
    std::vector<Network::PolicyVertexPair> initial_node_list;


    // m_expand_state acts as the lock for m_children.
    // see manipulation methods below for possible state transition
    enum class ExpandState : std::uint8_t {
        // initial state, no children
        INITIAL = 0,

        // creating children.  the thread that changed the node's state to
        // EXPANDING is responsible of finishing the expansion and then
        // move to EXPANDED, or revert to INITIAL if impossible
        EXPANDING,

        // expansion done.  m_children cannot be modified on a multi-thread
        // context, until node is destroyed.
        EXPANDED,
    };
    std::atomic<ExpandState> m_expand_state{ExpandState::INITIAL};

    // Tree data
    std::atomic<float> m_min_psa_ratio_children{2.0f};
    std::vector<UCTNodePointer> m_children;
    bool case_three;
    int case_three_move;
    int case_three_visit;
    float case_three_winrate;
    //  m_expand_state manipulation methods
    // INITIAL -> EXPANDING
    // Return false if current state is not INITIAL
    bool acquire_expanding();

    // EXPANDING -> DONE
    void expand_done();

    // EXPANDING -> INITIAL
    void expand_cancel();

    // wait until we are on EXPANDED state
    void wait_expanded();
};

#endif
