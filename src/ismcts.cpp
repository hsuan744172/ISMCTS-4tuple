#include "ismcts.hpp"

// ISMCTS 演算法的方向值常數
constexpr int ISMCTS::dir_val[4];

// =============================
// ISMCTS 類別建構子
// =============================
// 初始化模擬次數與隨機種子
ISMCTS::ISMCTS(int simulations) : simulations(simulations) {
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    rng.seed(static_cast<unsigned int>(seed));
}

// =============================
// 重置 ISMCTS 狀態
// =============================
// 重新設定隨機種子、清理根節點與排列統計
void ISMCTS::reset() {
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    rng.seed(static_cast<unsigned int>(seed));

    Node::cleanup(root);
    arrangement_stats.clear();
}

// =============================
// 取得確定化狀態
// =============================
// 根據原始狀態與目前迭代，隨機化未知棋子顏色
GST ISMCTS::getDeterminizedState(const GST &originalState, int current_iteration) {
    
    GST determinizedState = originalState;
    randomizeUnrevealedPieces(determinizedState, current_iteration);

    return determinizedState;
}

// =============================
// 隨機化未知顏色的棋子
// =============================
// 早期純隨機，後期根據統計加權
void ISMCTS::randomizeUnrevealedPieces(GST& state, int current_iteration) {
    const bool* revealed = state.get_revealed();
    std::vector<int> unrevealed_pieces;
    int redCount = 0, blueCount = 0;
    
    // 統計已揭示的紅藍棋子數量，並收集未知顏色的棋子
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

    // 後期才用統計資訊
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
    // 更新 arrangement_stats
    const auto& selected_arrangement = arrangements[selected_idx];
    for (size_t i = 0; i < unrevealed_pieces.size(); i++) {
        state.set_color(unrevealed_pieces[i], selected_arrangement[i]);
    }
}

// =============================
// 選擇階段
// =============================
// 根據 UCB 選擇最佳子節點
void ISMCTS::selection(Node*& node, GST& determinizedState) {
    while (!node->state.is_over() && !node->children.empty()) {
        Node* bestChild = nullptr;
        double bestUCB = -std::numeric_limits<double>::infinity();

        // 在確定化狀態上選擇最佳子節點
        for (auto& child : node->children) {
            if (child->visits == 0) {
                node = child.get();
                determinizedState.do_move(node->move);
                return;
            }
            double ucb = calculateUCB(child.get());
            if (ucb > bestUCB) {
                bestUCB = ucb;
                bestChild = child.get();
            }
        }
        
        if (bestChild) {
            // 更新節點和確定化狀態
            node = bestChild;
            determinizedState.do_move(node->move);
        } else break;
        
    }
}

// =============================
// 擴展階段
// =============================
// 對未結束節點產生所有合法子節點
void ISMCTS::expansion(Node *node, const GST &determinizedState) {
    if (node->state.is_over()) return;

    // 在確定化狀態上生成所有合法移動
    GST nodeState = determinizedState;

    int moves[MAX_MOVES];
    int moveCount = nodeState.gen_all_move(moves);

    for (int i = 0; i < moveCount; i++) {
        int move = moves[i];
        int piece = move >> 4;

        if (piece >= PIECES) continue;

        // 確保移動的棋子在合法範圍內
        int dir = move & 0xf;
        int dst = nodeState.get_pos(piece) + dir_val[dir];

        GST newState = nodeState;
        newState.do_move(move);
        std::unique_ptr<Node> newNode(new Node(newState, move));
        newNode->parent = node;
        node->children.push_back(std::move(newNode));
    }
}

// =============================
// 模擬階段
// =============================
// 隨機模擬遊戲直到結束，回傳勝負
// USER 端用 epsilon-greedy，ENEMY 隨機
// d: 資料物件，影響權重
// =============================
double ISMCTS::simulation(GST &state,DATA &d) {
    int moves[MAX_MOVES];
    int moveCount;
    GST simState = state;

    std::uniform_int_distribution<> dist(0, INT_MAX);

    int maxMoves = 1000;
    int moveCounter = 0;
    int Turn = ENEMY;

    // 模擬遊戲直到結束或達到最大移動次數
    while (!simState.is_over() && moveCounter < maxMoves) {
        moveCount = simState.gen_all_move(moves);
        if (moveCount == 0) break;

        int move;
        // 使用 epsilon-greedy 策略
        double epsilon = std::max(0.1, 1.0 - (double)moveCounter / 1000.0);
        std::uniform_real_distribution<> probDist(0.0, 1.0);

        // 根據當前輪次選擇移動
        if (Turn == USER) {
            // 使用 epsilon-greedy 策略
            double p = probDist(rng);
            if (p < epsilon) {
                int randomIndex = dist(rng) % moveCount;
                move = moves[randomIndex];
            } else {
                move = simState.highest_weight(d);// 考量4-tuple的模擬移動
            }
            Turn = ENEMY;
        } else {
            int randomIndex = dist(rng) % moveCount;
            move = moves[randomIndex];
            Turn = USER;
        }

        
        simState.do_move(move);
        moveCounter++;
    }

    // 若超過最大步數仍未結束，視為失敗
    if (!simState.is_over() && moveCounter >= maxMoves)
        return 0;

    int winner = simState.get_winner() == USER ? 1 : -1;

    return winner;
}

// =============================
// 反向傳播階段
// =============================
// 將模擬結果回傳至路徑上的所有節點
void ISMCTS::backpropagation(Node *node, double result) {
    while (node != nullptr){
        node->visits++;
        node->wins += result ;
        result = -result;  // 在每一層交替結果
        node = node->parent;
    }
}

// =============================
// 計算 UCB 值
// =============================
// UCB = 勝率 + 探索項
// 若未訪問則回傳無窮大
// =============================
double ISMCTS::calculateUCB(const Node *node) const {
    if (node->visits == 0) return std::numeric_limits<double>::infinity();
    
    double winRate = static_cast<double>(node->wins) / node->visits;
    double exploration = EXPLORATION_PARAM * std::sqrt(std::log(node->parent->visits) / node->visits);

    // UCB公式：利用 + 探索
    return winRate + exploration;
}

// =============================
// 尋找最佳移動 (AI主程式)
// =============================
// 進行多次模擬，選出訪問次數最多的子節點
int ISMCTS::findBestMove(GST &game, DATA &d) {
    Node::cleanup(root);
    root.reset(new Node(game));
    arrangement_stats.clear();

    for (int i = 0; i < simulations; i++) {
        Node* currentNode = root.get();
        
        // 獲取確定化狀態
        GST determinizedState = getDeterminizedState(game, i);

        // 選擇階段
        selection(currentNode, determinizedState);

        // 如果節點沒有子節點且遊戲未結束，進行擴展
        if (currentNode->children.empty() && !currentNode->state.is_over()) {
            expansion(currentNode, determinizedState);
        }

        // 確保有子節點可選擇
        Node* nodeToSimulate = currentNode;
        GST simulationState = determinizedState;

        // 隨機選擇一個子節點進行模擬
        if (!currentNode->children.empty()) {
            std::uniform_int_distribution<> dist(0, currentNode->children.size() - 1);
            int randomIndex = dist(rng);
            auto it = std::next(currentNode->children.begin(), randomIndex);
            nodeToSimulate = it->get();

            // 在確定化狀態上執行這個 move
            simulationState.do_move(nodeToSimulate->move);
        }
        int result = simulation(simulationState, d);
        
        // 更新 arrangement_stats
        std::string arrangementKey;
        const bool* revealed = game.get_revealed();
        for (int i = PIECES; i < PIECES*2; i++) {
            if (!revealed[i]) {
                int color = determinizedState.get_color(i);
                arrangementKey += (color == -RED ? 'R' : 'B');
            }
        }
        auto& stats = arrangement_stats[arrangementKey];
        if (result > 0) stats.first += 1;   // 勝場數
        stats.second += 1;                  // 模擬次數
        // 反向傳播結果
        backpropagation(nodeToSimulate, result);
    }

    Node* bestChild = nullptr;
    int maxVisits = -1;
    const char *dirNames[] = {"N","W","E","S"};
    bool hasValidMoves = false;

    // 尋找訪問次數最多的子節點(最佳解)
    for (auto& child : root->children) {
        if (child->visits > maxVisits) {
            maxVisits = child->visits;
            bestChild = child.get();
            hasValidMoves = true;
        }
    }
    if (!hasValidMoves) {
        fprintf(stderr, "No valid moves found. This might indicate the game is already over.\n");
        return -1;
    }
    
    // 輸出最佳子節點的資訊
    if (bestChild) {
        int piece = bestChild->move >> 4;
        int direction = bestChild->move & 0xf;

        fprintf(stderr, "piece: %d direction: %d\n", piece, direction);
        fprintf(stderr, "Win Rate: %.2f%%\n", bestChild->visits > 0 ? static_cast<double>(bestChild->wins) / bestChild->visits * 100 : 0.0);
    }

    fprintf(stderr, "return: %d\n", bestChild->move);

    return bestChild->move;
}