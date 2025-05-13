#ifndef ISMCTS_HPP
#define ISMCTS_HPP
#include "4T_GST.hpp"  
// #include "gst.hpp"
#include "node.hpp" 
#include <vector>
#include <memory>
#include <random>
#include <cmath>
#include <unordered_map> 
class ISMCTS {
private:
    int simulations;
    std::mt19937 rng;
    std::unique_ptr<Node> root; // 添加根節點成員


    void selection(Node*& node);
    void expansion(Node* node, const GST& determinizedState);
    int simulation(GST& state);
    void backpropagation(Node* node, double result);
    double calculateUCB(const Node* node) const;
    GST getDeterminizedState(const GST& originalState, int current_iteration);
    void randomizeUnrevealedPieces(GST& state, int current_iteration);
    std::unordered_map<std::string, std::pair<int, int>> arrangement_stats;


    // 模擬移動相關的常量
    static constexpr int dir_val[4] = {-6, -1, 1, 6}; // 上、左、右、下

public:
    ISMCTS(int simulations);
    void reset();
    int findBestMove(GST& game);
    void printNodeStats(const Node* node, int indent = 0) const;
};
#endif // ISMCTS_HPP
