# V12 IR Design

## Sea-of-Nodes

V12 uses a Sea-of-Nodes IR (after Cliff Click's 1995 paper), similar to
V8 TurboFan and Graal.

### Why Sea-of-Nodes over SSA?

In traditional SSA, instructions live inside basic blocks. The CFG is
the primary structure. In Sea-of-Nodes, every instruction is a node
and dependencies are encoded as edges:

- **Control edges**: which control-flow node does this node belong to?
- **Effect edges**: what memory state does this node depend on?
- **Value edges**: what values does this node consume?

This means nodes can "float" freely — they're not pinned to a basic
block. Only control and effect edges constrain their placement. This
makes many optimizations simpler:

- **GVN**: just hash-cons nodes with the same inputs.
- **DCE**: remove any node with no uses that isn't a control/effect sink.
- **LICM**: hoist a node out of a loop by changing its control edge.

### Node structure

```cpp
class Node {
    Graph* graph_;
    NodeId id_;
    Opcode op_;
    uint32_t bitset_;    // properties (pure, control, effect, etc.)
    Type type_;
    Inputs inputs_;      // [control?, effect?, value_inputs...]
    Uses uses_;          // reverse edges
};
```

### Input convention

The first input is the control input (for non-pure nodes). The second
is the effect input (for nodes with side effects). Remaining inputs are
value inputs.

For example, a `StoreField` node has:
- input[0]: control (which basic block)
- input[1]: effect (previous memory operation)
- input[2]: object (the receiver)
- input[3]: value (the value to store)

### Node properties

Each node has a bitset of properties:

- `kControl` — participates in control flow
- `kEffect` — has side effects
- `kPure` — no side effects (can be CSE'd/GVN'd)
- `kCommutative` — a OP b == b OP a
- `kCanDeoptimize` — may trigger deopt
- `kCanThrow` — may throw an exception

## Type system

Types are a bitset lattice:

```
                  Top (Any)
                /  |    |  \
            Number Object String Boolean ...
            /  |   |
          Int Float ...
```

Types drive:
- **Representation selection**: Int32 vs Float64 vs Tagged
- **Speculation guard insertion**: CheckSmi if type is Number|Undefined
- **Operation specialization**: JSAdd → Int32Add when both inputs are Int32

## IR verifier

The verifier checks these invariants after every pass:

1. **No null inputs**: every input is non-null and not dead.
2. **Use-def consistency**: if A is in B's use list, then B is in A's inputs.
3. **Def-use consistency**: if A is in B's inputs, then B is in A's use list.
4. **No dead nodes in use lists**: killed nodes are removed from all use lists.

The verifier aborts on failure. This catches the vast majority of IR
corruption bugs that would otherwise produce wrong code.
