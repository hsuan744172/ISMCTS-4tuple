#include "ismcts.hpp"
// #include "4T_GST.hpp"
// #include "4T_DATA.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cassert>

constexpr int ISMCTS::dir_val[4];

// ISMCTS實現
ISMCTS::ISMCTS(int simulations) : simulations(simulations)
{
    // 使用當前時間作為隨機數生成器的種子
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    rng.seed(static_cast<unsigned int>(seed));
}

// 初始化
void ISMCTS::reset()
{
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    rng.seed(static_cast<unsigned int>(seed));

    Node::cleanup(root);
    arrangement_stats.clear();
}

// 隨機化敵方棋子顏色 (只要棋子還活著就重新隨機化，但保留已知信息)
GST ISMCTS::getDeterminizedState(const GST &originalState, int current_iteration) 
{
    
    GST determinizedState = originalState;
    randomizeUnrevealedPieces(determinizedState, current_iteration);

    return determinizedState;
}

// 隨機化未揭露的敵方棋子
void ISMCTS::randomizeUnrevealedPieces(GST& state, int current_iteration) {
    const bool* revealed = state.get_revealed();
    std::vector<int> unrevealed_pieces;
    int redCount = 0, blueCount = 0;
    
    // 收集未揭露棋子
    for (int i = PIECES; i < PIECES*2; i++) {
        if (revealed[i]) {
            if (state.get_color(i) == -RED) redCount++;
            else blueCount++;
        } else {
            unrevealed_pieces.push_back(i);
        }
    }

    int totalRed = 4;
    int totalBlue = 4;
    int redRemaining = totalRed - redCount;
    int blueRemaining = totalBlue - blueCount;

    if (unrevealed_pieces.empty()) return;

    bool use_stats = (current_iteration >= simulations / 2);

    if (!use_stats) {
        // 早期：純隨機確定化 + 紀錄排列字串（在 simulation() 結束後更新）
        std::shuffle(unrevealed_pieces.begin(), unrevealed_pieces.end(), rng);
        for (size_t i = 0; i < unrevealed_pieces.size(); i++) {
            int piece = unrevealed_pieces[i];
            if (i < redRemaining) {
                state.set_color(piece, -RED);
            } else {
                state.set_color(piece, -BLUE);
            }
        }
        return;
    }

    // 後期：用 arrangement_stats 來推測
    // 生成所有合法排列
    std::vector<std::vector<int>> arrangements;
    int total_pieces = redRemaining + blueRemaining;
    int total_combinations = 1 << total_pieces;

    for (int mask = 0; mask < total_combinations; mask++) {
        std::vector<int> arrangement;
        int red = 0, blue = 0;
        for (int i = 0; i < total_pieces; i++) {
            if (mask & (1 << i)) {
                arrangement.push_back(-RED);
                red++;
            } else {
                arrangement.push_back(-BLUE);
                blue++;
            }
        }
        if (red == redRemaining && blue == blueRemaining) {
            arrangements.push_back(arrangement);
        }
    }

    // 計算每個排列的推測勝率
    std::vector<double> win_rates;
    for (const auto& arrangement : arrangements) {
        std::string key;
        for (auto color : arrangement) {
            key += (color == -RED) ? 'R' : 'B';
        }

        auto it = arrangement_stats.find(key);
        if (it == arrangement_stats.end() || it->second.second == 0) {
            win_rates.push_back(0.5);
        } else {
            double win_rate = static_cast<double>(it->second.first) / it->second.second;
            win_rates.push_back(win_rate);
        }
    }

    // 根據反勝率加權
    std::vector<double> weights;
    double total_weight = 0.0;
    for (double rate : win_rates) {
        double weight = 1.0 - rate + 0.05;
        weights.push_back(weight);
        total_weight += weight;
    }

    for (auto& w : weights) {
        w /= total_weight;
    }

    // 加權隨機選排列
    std::uniform_real_distribution<> dist(0.0, 1.0);
    double r = dist(rng);
    double cumulative = 0.0;
    int selected_idx = 0;
    for (size_t i = 0; i < weights.size(); i++) {
        cumulative += weights[i];
        if (r <= cumulative) {
            selected_idx = i;
            break;
        }
    }

    const auto& selected_arrangement = arrangements[selected_idx];
    for (size_t i = 0; i < unrevealed_pieces.size(); i++) {
        state.set_color(unrevealed_pieces[i], selected_arrangement[i]);
    }
}



void ISMCTS::selection(Node *&node)
{
    // 基於UCB選擇子節點，直到達到葉節點
    while (!node->state.is_over() && !node->children.empty())
    {
        Node *bestChild = nullptr;
        double bestUCB = -std::numeric_limits<double>::infinity();

        for (auto &child : node->children)
        {
            if (child->visits == 0)
            {
                node = child.get();
                return;
            }
            double ucb = calculateUCB(child.get());
            if (ucb > bestUCB)
            {
                bestUCB = ucb;
                bestChild = child.get();
            }
        }

        if (bestChild)
            node = bestChild;
        else
            break;
    }
}

void ISMCTS::expansion(Node *node, const GST &determinizedState)
{
    if (node->state.is_over())
        return;

    // 確保我們使用確定性狀態進行擴展
    GST nodeState = determinizedState;

    int moves[MAX_MOVES];
    int moveCount = nodeState.gen_all_move(moves);

    for (int i = 0; i < moveCount; i++)
    {
        int move = moves[i];
        int piece = move >> 4;
        int dir = move & 0xf;
        int dst = nodeState.get_pos(piece) + dir_val[dir];

        GST newState = nodeState;
        newState.do_move(move);
        std::unique_ptr<Node> newNode(new Node(newState, move));
        newNode->parent = node;
        node->children.push_back(std::move(newNode));
    }
}
int ISMCTS::simulation(GST& state) {
    int moves[MAX_MOVES];
    int moveCount;
    GST simState = state; // 複製狀態，不修改原始數據
    
    std::uniform_int_distribution<> dist(0, INT_MAX);
    
    // 記錄這次模擬確定化時的未揭露排列
    std::string arrangement_key;
    const bool* revealed = state.get_revealed();
    for (int i = PIECES; i < PIECES*2; i++) {
        if (!revealed[i]) {
            arrangement_key += (state.get_color(i) == -RED) ? 'R' : 'B';
        }
    }
    
    // 防止無限循環
    int maxMoves = 1000;
    int moveCounter = 0;
    while (!simState.is_over() && moveCounter < maxMoves) {
        moveCount = simState.gen_all_move(moves);
        if (moveCount == 0) break;

        int randomIndex = dist(rng) % moveCount;
        int move = moves[randomIndex];
        // simulationMoves.push_back(move); 
        simState.do_move(move);
        moveCounter++;
    }
    

    // 判斷遊戲結果
    int result;
    if (!simState.is_over() && moveCounter >= maxMoves) {
        result = 0; // 平局
    } else {
        result = (simState.get_winner() == USER) ? 1 : -1;
    }
    
    // 更新排列統計資料
    if (!arrangement_key.empty()) {
        auto& stat = arrangement_stats[arrangement_key];
        if (result == 1) { 
            stat.first++;  // 勝場+1
        }
        stat.second++;      // 總場數+1
    }
    
    return result;
}

// double ISMCTS::simulation(GST &state)
// {
//     int moves[MAX_MOVES];
//     int moveCount;
//     GST simState = state; // 複製狀態，不修改原始數據

//     std::uniform_int_distribution<> dist(0, INT_MAX);
    // std::vector<int> simulationMoves; // 記錄走步

    // int maxMoves = 1000;
    // int moveCounter = 0;

    // while (!simState.is_over() && moveCounter < maxMoves)
    // {
    //     moveCount = simState.gen_all_move(moves);
    //     if (moveCount == 0) break;

    //     int randomIndex = dist(rng) % moveCount;
    //     int move = moves[randomIndex];
    //     // simulationMoves.push_back(move); 
    //     simState.do_move(move);
    //     moveCounter++;
    // }

    // if (!simState.is_over() && moveCounter >= maxMoves)
    //     return 0;

    // int winner = simState.get_winner() == USER ? 1 : -1;

    // 輸出模擬盤面
    // if (simState.is_escape && simState.get_winner() == ENEMY)
    // {
    //     std::cout << "逃脫獲勝走步：\n";
    //     for (int move : simulationMoves)
    //     {
    //         int piece = move >> 4;
    //         int dir = move & 0xf;
    //         const char *dirNames[] = {"上", "左", "右", "下"};
    //         if (piece < PIECES)
    //             std::cout << static_cast<char>('A' + piece % PIECES);
    //         else
    //             std::cout << static_cast<char>('a' + (piece - PIECES) % PIECES);
    //         std::cout << " " << dirNames[dir] << "\n";
    //     }
    //     std::cout << "moveCounter: " << moveCounter << "\n\n";
    //     simState.print_board();

    //     system("pause");
    // }

//     return winner;
// }

void ISMCTS::backpropagation(Node *node, double result)
{
    while (node != nullptr)
    {
        node->visits++;
        node->wins += result;
        node = node->parent;
    }
}

double ISMCTS::calculateUCB(const Node *node) const
{
    if (node->visits == 0) return std::numeric_limits<double>::infinity();
    

    double winRate = static_cast<double>(node->wins) / node->visits;

    double exploration = EXPLORATION_PARAM * std::sqrt(std::log(node->parent->visits) / node->visits);

    // UCB公式：利用 + 探索
    return winRate + exploration;
}

void ISMCTS::printNodeStats(const Node *node, int indent) const
{
    std::string indentation(indent * 2, ' ');

    // Print current node information
    int piece = node->move >> 4;
    int direction = node->move & 0xf;
    const char *dirNames[] = {"N", "W", "E", "S"};

    if (node->move != -1)
    {
        std::cout << indentation << "Move: ";
        if (piece < PIECES)
            std::cout << static_cast<char>('A' + piece % PIECES);
        else
            std::cout << static_cast<char>('a' + (piece - PIECES) % PIECES);

        std::cout << " " << dirNames[direction];
        std::cout << " - Wins/Visits: " << node->wins << "/" << node->visits;
        std::cout << " = " << std::fixed << std::setprecision(2)
                  << (node->visits > 0 ? static_cast<double>(node->wins) / node->visits : 0) << std::endl;
    }
    else
    {
        std::cout << indentation << "Root Node - Wins/Visits: " << node->wins << "/" << node->visits << std::endl;
    }

    if (node->visits >= 10)
    {
        for (const auto &child : node->children)
        {
            if (child->visits > 0) printNodeStats(child.get(), indent + 1);
            
        }
    }
}
int ISMCTS::findBestMove(GST& game) {
    if (game.is_over()) {
        return -1;
    }

    // 清理之前的搜尋樹
    Node::cleanup(root);
    
    // 創建新的根節點 (改用 new 而不是 make_unique)
    root.reset(new Node(game));
    arrangement_stats.clear();
    
    for (int i = 0; i < simulations; i++) {
        Node* currentNode = root.get();
        GST determinizedState = getDeterminizedState(game,i);

        // 選擇階段
        selection(currentNode);
        
        // 如果節點沒有子節點且遊戲未結束，進行擴展
        if (currentNode->children.empty() && !currentNode->state.is_over()) {
            expansion(currentNode, determinizedState);
        }
        
        // 確保有子節點可選擇
        Node* nodeToSimulate = currentNode;
        if (!currentNode->children.empty()) {
            // 隨機選擇一個子節點進行模擬
            std::uniform_int_distribution<> dist(0, currentNode->children.size() - 1);
            int randomIndex = dist(rng);
            auto it = std::next(currentNode->children.begin(), randomIndex);
            nodeToSimulate = it->get();
        }

        // 一定要進行模擬
        GST simulationState = nodeToSimulate->state;
        int result = simulation(simulationState);
        
        // 反向傳播結果
        backpropagation(nodeToSimulate, result);
    }

    Node* bestChild = nullptr;
    int maxVisits = -1;
    
    for (auto& child : root->children) {
        if (child->visits > maxVisits) {
            maxVisits = child->visits;
            bestChild = child.get();
        }
    }

    return bestChild ? bestChild->move : -1;
}

// int ISMCTS::findBestMove(GST &game)
// {
//     if (game.is_over())
//     {
//         std::cout << "Game is already over. Winner: " << (game.get_winner() == USER ? "User" : "Enemy") << std::endl;
//         return -1;
//     }

//     // 清理之前的搜尋樹
//     Node::cleanup(root);

//     // 創建新的根節點 (改用 new 而不是 make_unique)
//     root.reset(new Node(game));
//     arrangement_stats.clear();

//     for (int i = 0; i < simulations; i++)
//     {
//         Node *currentNode = root.get();
//         GST determinizedState = getDeterminizedState(game, i);

//         // 選擇階段
//         selection(currentNode);

//         // 如果節點沒有子節點且遊戲未結束，進行擴展
//         if (currentNode->children.empty() && !currentNode->state.is_over())
//         {
//             expansion(currentNode, determinizedState);
//         }

//         // 確保有子節點可選擇
//         Node *nodeToSimulate = currentNode;
//         if (!currentNode->children.empty())
//         {
//             // 隨機選擇一個子節點進行模擬
//             std::uniform_int_distribution<> dist(0, currentNode->children.size() - 1);
//             int randomIndex = dist(rng);
//             auto it = std::next(currentNode->children.begin(), randomIndex);
//             nodeToSimulate = it->get();
//         }

//         // 一定要進行模擬
//         GST simulationState = nodeToSimulate->state;
//         int result = simulation(simulationState);

//         // 反向傳播結果
//         backpropagation(nodeToSimulate, result);
//     }

//     Node *bestChild = nullptr;
//     int maxVisits = -1;

//     // Reverse the direction names to match the AI's perspective
//     const char *dirNames[] = {"S", "E", "W", "N"};

//     // Print detailed statistics for each move
//     std::cout << "ISMCTS Decision Statistics:\n";
//     bool hasValidMoves = false;

//     for (auto &child : root->children)
//     {
//         int piece = child->move >> 4;
//         int direction = child->move & 0xf;

//         std::cout << "移動 ";
//         if (piece < PIECES)
//             std::cout << static_cast<char>('A' + piece % PIECES);
//         else
//             std::cout << static_cast<char>('a' + (piece - PIECES) % PIECES);

//         std::cout << " " << dirNames[direction] << ": "
//                   << child->wins << "/" << child->visits
//                   << " = " << std::fixed << std::setprecision(2)
//                   << (child->visits > 0 ? static_cast<double>(child->wins) / child->visits : 0.0)
//                   << std::endl;

//         if (child->visits > maxVisits)
//         {
//             maxVisits = child->visits;
//             bestChild = child.get();
//             hasValidMoves = true;
//         }
//     }

//     if (!hasValidMoves)
//     {
//         std::cout << "No valid moves found. This might indicate the game is already over." << std::endl;
//         return -1;
//     }

//     if (bestChild)
//     {
//         int piece = bestChild->move >> 4;
//         int direction = bestChild->move & 0xf;

//         std::cout << "\n選擇最佳移動: ";
//         if (piece < PIECES)
//             std::cout << static_cast<char>('A' + piece % PIECES);
//         else
//             std::cout << static_cast<char>('a' + (piece - PIECES) % PIECES);

//         std::cout << " " << dirNames[direction] << std::endl;
//         std::cout << "勝率: " << std::fixed << std::setprecision(2)
//                   << (bestChild->visits > 0 ? static_cast<double>(bestChild->wins) / bestChild->visits * 100 : 0.0)
//                   << "%" << std::endl;
//     }

//     return bestChild ? bestChild->move : -1;
// }