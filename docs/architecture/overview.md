# V12 Architecture

## Overview

V12 is a JavaScript JIT compiler organized around these principles:

1. **Tiered execution**: interpreter → baseline JIT → optimizing JIT
2. **Sea-of-Nodes IR**: the central representation for optimizations
3. **Adapter boundaries**: external libraries (regalloc, assembler) plug in
   through stable contracts; the core compiler never depends on them directly
4. **Aggressive verification**: IR verifier runs after every pass

## Component dependency graph

```
                       frontend
                          │
                          ▼
                      bytecode
                          │
                          ▼
                    interpreter
                          │
                  feedback/profiling
                          │
                          ▼
                     graph builder
                          │
                          ▼
                    Sea-of-Nodes IR
                          │
              ┌───────────┴───────────┐
              │                       │
         JS analyses             speculation
              │                       │
              └───────────┬───────────┘
                          ▼
                     optimizer
                          │
                          ▼
                      lowering
                          │
                          ▼
                     Machine IR
                          │
                    ┌─────┴─────┐
                    ▼           ▼
                 regalloc    diagnostics
                    │
                    ▼
                code emitter
                    │
                    ▼
                 CodeObject
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
        GC/maps            deopt
```

## The contracts layer

The `src/contracts/` directory defines the stable internal interfaces:

- `machine-ir.h` — MachineFunction, MachineBlock, MachineInstruction, VReg, AllocationResult
- `regalloc-interface.h` — RegisterAllocator abstract class
- `assembler-interface.h` — MachineEmitter abstract class
- `target-description.h` — TargetDescription abstract class
- `code-object.h` — CodeObject (final output)

External libraries plug into these contracts through adapters in
`src/codegen/`. The optimizer and lowering layers only see the contracts,
never the library-specific APIs.

## Tiering

### Tier 0: Interpreter

The bytecode interpreter executes FunctionInfo bytecode. It records type
feedback into a FeedbackVector. When a function's hotness counter exceeds
a threshold, the function is queued for baseline compilation.

### Tier 1: Baseline JIT

The baseline compiler generates machine code directly from bytecode,
without building an IR. It's fast to compile but produces unoptimized
code. Inline caches are embedded in the generated code.

### Tier 2: Optimizing JIT

The optimizing compiler builds a Sea-of-Nodes IR from bytecode + type
feedback, runs optimization passes (GVN, LICM, inlining, escape analysis,
etc.), lowers to Machine IR, runs register allocation, and emits machine
code.

If speculation fails at runtime, the code deoptimizes back to the
interpreter.

## Deoptimization

Every speculative operation in the IR has an associated FrameState node
that captures the interpreter state needed to reconstruct execution.
When a speculation fails (e.g. a CheckSmi fails because the value is
actually a HeapNumber), the deoptimizer:

1. Reads the FrameState at the deopt point.
2. Materializes any escaped objects that were scalar-replaced.
3. Translates the machine state back to interpreter registers.
4. Jumps to the interpreter at the correct bytecode offset.

## GC

The GC is a stop-the-world mark-sweep collector. The heap is bump-pointer
allocated in chunks. Safepoints in generated code allow the GC to scan
the stack for roots.

Write barriers track inter-generation pointers (when we add generational
GC in the future).

## Testing strategy

- **Unit tests**: each component has its own test file.
- **IR verifier**: runs after every optimization pass.
- **Sanitizer builds**: ASan, UBSan, LSan, TSan in CI.
- **Differential testing**: (planned) compare interpreter vs JIT output.
- **Golden tests**: (planned) compare IR dumps against expected output.
- **Fuzzing**: (planned) libFuzzer harnesses for parser, IR, codegen.
