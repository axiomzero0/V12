// =============================================================================
// src/ir/graph/graph.cc
// =============================================================================

#include "ir/graph/graph.h"

#include <cstdio>
#include <unordered_set>

namespace v12 {

Node* Graph::NewNode(Opcode op, uint32_t bitset, Type type,
                     Node* control, Node* effect,
                     Node::Inputs&& value_inputs) {
    Node* n = arena_->New<Node>(this, op, bitset, type, control, effect,
                                std::move(value_inputs));
    n->id_ = next_id_++;

    // Insert at head of intrusive list (creation order is reverse-program-order).
    if (first_ == nullptr) {
        first_ = last_ = n;
    } else {
        last_->next_ = n;
        last_ = n;
    }
    return n;
}

Node* Graph::NewNode1(Opcode op, uint32_t bitset, Type type,
                      Node* control, Node* effect, Node* input1) {
    Node::Inputs ins;
    ins.push_back(input1);
    return NewNode(op, bitset, type, control, effect, std::move(ins));
}

Node* Graph::NewNode2(Opcode op, uint32_t bitset, Type type,
                      Node* control, Node* effect,
                      Node* input1, Node* input2) {
    Node::Inputs ins;
    ins.push_back(input1);
    ins.push_back(input2);
    return NewNode(op, bitset, type, control, effect, std::move(ins));
}

Node* Graph::NewPureNode(Opcode op, uint32_t bitset, Type type,
                         Node::Inputs&& inputs) {
    return NewNode(op, bitset, type, nullptr, nullptr, std::move(inputs));
}

void Graph::Verify() {
    VerifyGraph(this);
}

void Graph::Dump() {
    std::fprintf(stderr, "=== Graph Dump (%d nodes) ===\n", next_id_);
    for (Node* n = first_; n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        std::fprintf(stderr, "N%u: %s  type=", n->id(), n->op_name());
        // Type dump omitted for brevity - see ir-dumper for full format.
        std::fprintf(stderr, "  inputs=[");
        for (int i = 0; i < n->input_count(); ++i) {
            if (i > 0) std::fprintf(stderr, ",");
            Node* in = n->input(i);
            std::fprintf(stderr, "N%u", in ? in->id() : 0xFFFFFFFF);
        }
        std::fprintf(stderr, "]  uses=%d\n", n->use_count());
    }
    std::fprintf(stderr, "=== End Graph Dump ===\n");
}

void VerifyGraph(Graph* g) {
    // Invariant 1: every input of a node is non-null and not dead.
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        for (int i = 0; i < n->input_count(); ++i) {
            V12_CHECK(n->input(i) != nullptr,
                      "VerifyGraph: N%u has null input %d", n->id(), i);
            V12_CHECK(!n->input(i)->IsDead(),
                      "VerifyGraph: N%u input %d is dead", n->id(), i);
        }
    }

    // Invariant 2: every use is consistent (the use-list of A contains B
    // iff B has A as an input).
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        for (Node* u : n->uses()) {
            V12_CHECK(!u->IsDead(), "VerifyGraph: dead node in use list of N%u", n->id());
            bool found = false;
            for (int i = 0; i < u->input_count(); ++i) {
                if (u->input(i) == n) { found = true; break; }
            }
            V12_CHECK(found, "VerifyGraph: N%u claims use N%u but N%u has no input from N%u",
                      n->id(), u->id(), u->id(), n->id());
        }
    }

    // Invariant 3: every node in use-list is reachable from inputs.
    for (Node* n = g->first_node(); n != nullptr; n = n->next_in_graph()) {
        if (n->IsDead()) continue;
        for (int i = 0; i < n->input_count(); ++i) {
            Node* in = n->input(i);
            bool found = false;
            for (Node* u : in->uses()) {
                if (u == n) { found = true; break; }
            }
            V12_CHECK(found, "VerifyGraph: N%u input %d (N%u) does not have N%u in its use list",
                      n->id(), i, in->id(), n->id());
        }
    }
}

}  // namespace v12
