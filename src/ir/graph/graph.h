// =============================================================================
// src/ir/graph/graph.h
// =============================================================================
// Graph: owns all Nodes in a function's IR.
//
// The Graph is a node factory + a node container. It allocates Nodes from
// an arena (so they're contiguous-ish and cache-friendly) and maintains an
// intrusive list of all live nodes.
//
// The Graph also owns the "next node id" counter and provides iteration.

#ifndef V12_IR_GRAPH_GRAPH_H_
#define V12_IR_GRAPH_GRAPH_H_

#include <cstdint>

#include "base/arena.h"
#include "base/macros.h"
#include "ir/graph/node.h"
#include "ir/types/type.h"

namespace v12 {

class Graph {
public:
    explicit Graph(Arena* arena) : arena_(arena) {}

    Arena* arena() { return arena_; }

    // Create a node with the given opcode, properties, type, and inputs.
    // The node is owned by the graph.
    Node* NewNode(Opcode op, uint32_t bitset, Type type,
                  Node* control, Node* effect,
                  Node::Inputs&& value_inputs);

    // Convenience: 1-value-input node.
    Node* NewNode1(Opcode op, uint32_t bitset, Type type,
                   Node* control, Node* effect, Node* input1);

    // Convenience: 2-value-input node.
    Node* NewNode2(Opcode op, uint32_t bitset, Type type,
                   Node* control, Node* effect,
                   Node* input1, Node* input2);

    // Convenience: pure node (no control/effect).
    Node* NewPureNode(Opcode op, uint32_t bitset, Type type, Node::Inputs&& inputs);

    // Number of nodes ever created (including dead ones).
    int node_count() const { return next_id_; }

    // Iteration: intrusive list. The list is in creation order, which is
    // roughly reverse-program-order because we build the graph bottom-up.
    Node* first_node() const { return first_; }
    Node* last_node() const { return last_; }

    // Get the Start node (the root of the control flow).
    Node* start() const { return start_; }
    void set_start(Node* n) { start_ = n; }

    // Get the End node (the single sink of control flow).
    Node* end() const { return end_; }
    void set_end(Node* n) { end_ = n; }

    // Verify the graph's invariants. Aborts on failure.
    void Verify();

    // Dump the graph to a textual representation (for --dump-ir).
    void Dump();

private:
    Arena* arena_;
    Node* first_ = nullptr;
    Node* last_ = nullptr;
    Node* start_ = nullptr;
    Node* end_ = nullptr;
    NodeId next_id_ = 0;
};

// Graph verifier: checks invariants.
//   - Every input of a node is non-null and not dead.
//   - Every use is consistent with inputs.
//   - Control inputs form a DAG (no cycles) except for loop back-edges.
//   - Effect inputs form a DAG.
void VerifyGraph(Graph* g);

}  // namespace v12

#endif  // V12_IR_GRAPH_GRAPH_H_
