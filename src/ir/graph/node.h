// =============================================================================
// src/ir/graph/node.h
// =============================================================================
// IR Node - the fundamental unit of a Sea-of-Nodes graph.
//
// Design:
//   - Every operation is a Node.
//   - Nodes have a fixed arity of "input" edges (data dependencies).
//   - Nodes have a variable-length "use" list (reverse edges).
//   - Nodes have a Kind (what operation) and an Opcode (specific variant).
//   - Control flow (Start, End, Branch, Merge, Loop) is also represented
//     as nodes; control edges are the first input of a node.
//   - Effect edges (memory state) are the second input.
//   - This is the classic Sea-of-Nodes representation from Cliff Click's
//     1995 paper, as used by V8 TurboFan and Graal.
//
// Why Sea-of-Nodes over SSA?
//   - SSA requires basic blocks. Sea-of-Nodes encodes control dependencies
//     as edges, so nodes can float freely between blocks as long as their
//     control input is satisfied. This makes many optimizations (GVN, DCE,
//     LICM) much simpler.
//   - The trade-off is that we have to explicitly model control and effect
//     dependencies, which is more work than just using basic blocks.

#ifndef V12_IR_GRAPH_NODE_H_
#define V12_IR_GRAPH_NODE_H_

#include <cstdint>

#include "base/arena.h"
#include "base/bitset.h"
#include "base/macros.h"
#include "base/small-vector.h"
#include "ir/types/type.h"

namespace v12 {

class Graph;
class Node;
class Use;
class Edge;

// Node id. Unique within a Graph. Used for stable hashing and bitset indexing.
using NodeId = uint32_t;

// Opcode - the kind of operation. Each opcode has a fixed arity (number of
// input edges) and a fixed set of properties (pure, control, effect, etc.).
// See ir/nodes/ for the actual node definitions.
enum class Opcode : uint16_t {
    // Control nodes
    kStart,
    kEnd,
    kBranch,            // if (cond) goto true_target else false_target
    kSwitch,
    kMerge,
    kLoop,
    kLoopExit,
    kReturn,
    kUnreachable,
    kThrow,

    // Common
    kParameter,         // function parameter (input: Start)
    kConstant,          // a constant value
    kPhi,               // SSA phi node
    kEffectPhi,         // phi for effect chain
    kControlPhi,

    // Arithmetic - integer
    kInt32Constant,
    kInt64Constant,
    kInt32Add,
    kInt32Sub,
    kInt32Mul,
    kInt32Div,
    kInt32Mod,
    kInt32Neg,
    kInt32Abs,
    kShiftLeft,
    kShiftRight,
    kShiftRightLogical,
    kBitwiseAnd,
    kBitwiseOr,
    kBitwiseXor,
    kBitwiseNot,

    // Arithmetic - float
    kFloat64Constant,
    kFloat64Add,
    kFloat64Sub,
    kFloat64Mul,
    kFloat64Div,
    kFloat64Mod,
    kFloat64Neg,
    kFloat64Abs,
    kFloat64Sqrt,
    kFloat64Floor,
    kFloat64Ceil,
    kFloat64Trunc,
    kFloat64Round,

    // Conversions
    kChangeInt32ToFloat64,
    kChangeFloat64ToInt32,
    kChangeTaggedToFloat64,
    kChangeTaggedToInt32,
    kChangeTaggedToUint32,
    kChangeInt32ToTagged,
    kChangeFloat64ToTagged,
    kTruncateFloat64ToInt32,
    kCheckedTaggedToInt32,
    kCheckedTaggedToFloat64,

    // Comparison
    kWord32Equal,
    kWord64Equal,
    kInt32LessThan,
    kInt32LessThanOrEqual,
    kUint32LessThan,
    kUint32LessThanOrEqual,
    kFloat64Equal,
    kFloat64LessThan,
    kFloat64LessThanOrEqual,

    // JavaScript operations - before lowering
    kJSAdd,
    kJSSub,
    kJSMul,
    kJSDiv,
    kJSMod,
    kJSExp,
    kJSBitwiseAnd,
    kJSBitwiseOr,
    kJSBitwiseXor,
    kJSShiftLeft,
    kJSShiftRight,
    kJSShiftRightLogical,
    kJSNegate,
    kJSBitwiseNot,
    kJSIncrement,
    kJSDecrement,
    kJSEqual,
    kJSNotEqual,
    kJSStrictEqual,
    kJSStrictNotEqual,
    kJSLessThan,
    kJSGreaterThan,
    kJSLessThanOrEqual,
    kJSGreaterThanOrEqual,

    // Object operations
    kLoadField,
    kStoreField,
    kLoadElement,
    kStoreElement,
    kLoadProperty,      // generic property load (before IC specialization)
    kStoreProperty,
    kCheckShape,        // deopt if shape != expected
    kCheckMaps,         // alias for CheckShape
    kAllocateObject,
    kAllocateArray,
    kCheckBounds,       // deopt if index >= length
    kLoadLength,

    // Calls
    kCall,
    kCallJS,
    kCallBuiltin,
    kTailCall,

    // JavaScript-specific
    kJSCall,
    kJSConstruct,
    kJSLoadProperty,
    kJSStoreProperty,
    kJSLoadGlobal,
    kJSStoreGlobal,
    kJSCreateClosure,
    kJSCreateObject,
    kJSCreateArray,
    kJSToBoolean,
    kJSToNumber,
    kJSToString,
    kJSToObject,
    kJSForInPrepare,
    kJSForInNext,
    kJSYield,
    kJSAwait,

    // Type checks / speculation
    kCheckSmi,
    kCheckNumber,
    kCheckString,
    kCheckHeapObject,
    kCheckJSArray,
    kCheckFloat64Hole,
    kCheckNotTaggedHole,
    kCheckEquals,

    // Deoptimization
    kDeoptimizeIf,
    kDeoptimizeUnless,
    kDeoptimize,

    // Memory / effect
    kLoad,
    kStore,
    kProtectedLoad,
    kProtectedStore,

    // Frame state for deopt
    kFrameState,
    kStateValues,

    // Constants for runtime
    kExternalConstant,
    kNumberConstant,
    kHeapConstant,
    kOsrValue,

    // Misc
    kDebugBreak,
    kUnreachable2,
    kDead,
    kPrologue,

    kCount
};

const char* OpcodeName(Opcode op);

// Node properties (a bitmask). Used by passes to quickly decide whether
// a node is relevant without switching on Opcode.
namespace NodeProp {
constexpr uint32_t kControl   = 1u << 0;   // participates in control flow
constexpr uint32_t kEffect    = 1u << 1;   // has effect input/output
constexpr uint32_t kPure      = 1u << 2;   // no side effects (can be CSE'd/GVN'd)
constexpr uint32_t kCommutative = 1u << 3; // a OP b == b OP a
constexpr uint32_t kMemoryRead  = 1u << 4;
constexpr uint32_t kMemoryWrite = 1u << 5;
constexpr uint32_t kCall        = 1u << 6;
constexpr uint32_t kCanDeoptimize = 1u << 7;
constexpr uint32_t kCanThrow      = 1u << 8;
constexpr uint32_t kNoThrow       = 1u << 9;  // cannot throw (for call)
constexpr uint32_t kKilled          = 1u << 10; // marked dead by DCE
constexpr uint32_t kSimplified    = 1u << 11;  // already simplified
constexpr uint32_t kIdempotent    = 1u << 12;
}  // namespace NodeProp

// A node has a fixed maximum number of inputs. We reserve space inline.
// Most nodes have 1-3 inputs; Phis and FrameStates can have many.
constexpr int kMaxInlineInputs = 6;

class Node {
public:
    // Inputs:
    //   [0]: control input (for non-pure nodes)
    //   [1]: effect input (for nodes with effects)
    //   [2..]: value inputs
    //
    // We use a SmallVector so common cases don't heap-allocate.
    using Inputs = SmallVector<Node*, kMaxInlineInputs>;
    using Uses = SmallVector<Node*, 4>;

    Node(Graph* graph, Opcode op, uint32_t bitset, Type type,
         Node* control, Node* effect, Inputs value_inputs);

    NodeId id() const { return id_; }
    Opcode op() const { return op_; }
    const char* op_name() const { return OpcodeName(op_); }

    uint32_t bitset() const { return bitset_; }
    bool HasProp(uint32_t flag) const { return (bitset_ & flag) != 0; }
    void SetProp(uint32_t flag) { bitset_ |= flag; }
    void ClearProp(uint32_t flag) { bitset_ &= ~flag; }

    Type type() const { return type_; }
    void set_type(Type t) { type_ = t; }

    // ----- Inputs -----
    int input_count() const { return static_cast<int>(inputs_.size()); }
    Node* input(int i) const {
        V12_DCHECK(i >= 0 && i < input_count(), "input index out of range");
        return inputs_[i];
    }
    Node* control_input() const {
        return HasProp(NodeProp::kControl) ? inputs_[0] : nullptr;
    }
    Node* effect_input() const {
        return HasProp(NodeProp::kEffect) ? inputs_[HasProp(NodeProp::kControl) ? 1 : 0] : nullptr;
    }
    void SetInput(int i, Node* new_input);
    void AppendInput(Node* new_input);
    void InsertInput(int i, Node* new_input);
    void RemoveInput(int i);

    Inputs& inputs() { return inputs_; }
    const Inputs& inputs() const { return inputs_; }

    // ----- Uses (reverse edges) -----
    int use_count() const { return static_cast<int>(uses_.size()); }
    const Uses& uses() const { return uses_; }
    void AddUse(Node* n) { uses_.push_back(n); }
    void RemoveUse(Node* n);

    // ----- Replacement -----
    // Replace all uses of this node with `other`. This node becomes dead
    // (use_count == 0) but is not freed; the Graph still owns it.
    void ReplaceAllUsesWith(Node* other);

    // ----- Killing -----
    void Kill();  // mark as dead, detach from inputs

    bool IsDead() const { return HasProp(NodeProp::kKilled); }

    // ----- Diagnostics -----
    void Dump() const;

    // For Node-iteration over the graph.
    Node* next_in_graph() const { return next_; }
    void set_next_in_graph(Node* n) { next_ = n; }

    Graph* graph() const { return graph_; }

private:
    friend class Graph;

    Graph* graph_;
    NodeId id_;
    Opcode op_;
    uint32_t bitset_;
    Type type_;
    Inputs inputs_;
    Uses uses_;
    Node* next_ = nullptr;  // intrusive list of all nodes in the graph
};

}  // namespace v12

#endif  // V12_IR_GRAPH_NODE_H_
