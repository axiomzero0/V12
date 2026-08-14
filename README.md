# V12 — A JavaScript JIT Compiler

A from-scratch JavaScript JIT compiler with a Sea-of-Nodes IR, built in C++20.

## Architecture

V12 follows a tiered execution model:

```
Source → Lexer → Parser → AST → Bytecode Generator → Bytecode
                                                        ↓
                                              Interpreter (tier 0)
                                                        ↓ (hot function detected)
                                              Baseline JIT (tier 1)
                                                        ↓ (very hot)
                                              Optimizing JIT (tier 2)
                                                        ↓
                                              Sea-of-Nodes IR
                                                  ↓
                                              Optimizer Pipeline
                                                  ↓
                                              Lowering
                                                  ↓
                                              Machine IR (our own)
                                                  ↓
                                    ┌─────────────┴─────────────┐
                                    │ Regalloc Adapter          │
                                    │ (external RA library)     │
                                    └─────────────┬─────────────┘
                                                  ↓
                                    │ Machine Emitter Adapter   │
                                    │ (asmjit/Xbyak/etc.)       │
                                    └─────────────┬─────────────┘
                                                  ↓
                                              Code Object
                                                  ↓
                                              Execution
```

### Key design decisions

1. **We own the JavaScript intelligence, not the mechanical work.**
   - Machine code generation and register allocation are delegated to
     external libraries through adapter boundaries.
   - We own the Sea-of-Nodes IR, the Machine IR, and all JS-specific
     optimizations.

2. **Stable internal contracts.**
   - `src/contracts/` defines the interfaces that external libraries
     plug into.
   - The optimizer never sees assembler or regalloc APIs directly.

3. **Aggressive IR verification.**
   - The graph verifier runs after every major pass.
   - Invariants: use-def consistency, SSA uniqueness, dominance,
     type correctness.

4. **Sanitizer-first development.**
   - All code is tested under ASan + UBSan.
   - The CI matrix includes separate ASan, UBSan, LSan, and TSan jobs.

## Repository layout

See the top-level directory structure. Key directories:

- `src/base/` — arena, zone, bitset, small-vector, hash-map, tagged-value
- `src/frontend/` — lexer, parser, AST, bytecode
- `src/vm/` — isolate, values, objects, shapes, arrays
- `src/interpreter/` — bytecode interpreter
- `src/feedback/` — type feedback system
- `src/ic/` — inline caches
- `src/ir/` — Sea-of-Nodes IR (graph, nodes, types, builder)
- `src/optimizer/` — optimization passes (GVN, DCE, LICM, etc.)
- `src/lowering/` — IR to Machine IR lowering
- `src/codegen/` — code emission (adapter to external assembler)
- `src/contracts/` — stable internal interfaces (Machine IR, RA, emitter)
- `src/deopt/` — deoptimization
- `src/gc/` — garbage collector
- `src/diagnostics/` — IR dumper, disassembler, tracing

## Building

### Quick build (no CMake required)

```bash
./scripts/build.sh debug      # debug build
./scripts/build.sh release    # optimized build
./scripts/build.sh asan       # ASan + UBSan build
./scripts/build.sh tests      # build and run tests
./scripts/build.sh clean      # remove build artifacts
```

### CMake build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DV12_ENABLE_TESTS=ON
cmake --build build -j$(nproc)
```

CMake options:
- `V12_ENABLE_ASAN` — enable AddressSanitizer
- `V12_ENABLE_UBSAN` — enable UndefinedBehaviorSanitizer
- `V12_ENABLE_LSAN` — enable LeakSanitizer (standalone)
- `V12_ENABLE_TSAN` — enable ThreadSanitizer
- `V12_ENABLE_COVERAGE` — enable code coverage
- `V12_ENABLE_TESTS` — build tests (default ON)
- `V12_ENABLE_TOOLS` — build tools (default ON)
- `V12_WERROR` — treat warnings as errors

## Testing

```bash
./build/bin/test_runner
```

The test framework is a minimal gtest-compatible implementation in
`tests/test-framework.h`. When GoogleTest is available, the CMake build
uses it instead.

## Status

This is an early-stage project. The following components are implemented:

- ✅ Base infrastructure (arena, zone, bitset, small-vector, hash-map, tagged-value)
- ✅ Lexer (full JS token set)
- ✅ Parser (most JS syntax: functions, classes, control flow, expressions)
- ✅ AST with source positions
- ✅ Bytecode format and opcode definitions
- ✅ VM value model (TaggedValue, HeapObject, Shape, Context)
- ✅ Singleton primitives (Undefined, Null, True, False) properly per-Isolate
- ✅ Object model with backing-store property storage (shape transitions work correctly)
- ✅ Runtime helpers (ToBoolean, ToNumber, ToString, Add, comparison ops, etc.)
- ✅ HostFunction support (native C++ functions callable from JS)
- ✅ Scope analysis (variable resolution, closure capture detection)
- ✅ Bytecode generator (AST → register-based bytecode with register allocation)
- ✅ Register-based interpreter (tier 0) — runs real JS programs
- ✅ Closures (captured variables via heap-allocated Context chain)
- ✅ Try/catch exception handling (compile-time handler table)
- ✅ For-in iteration (object property names)
- ✅ For-of iteration (arrays, strings)
- ✅ Built-in functions: print, console.log, parseInt, parseFloat, isNaN,
  Array.isArray, Object.keys, String.fromCharCode, Math.{abs,floor,ceil,
  round,sqrt,pow,min,max,random,PI,E}
- ✅ String methods: charAt, substring/slice, toUpperCase, toLowerCase, indexOf
- ✅ Array methods: push, pop
- ✅ Sea-of-Nodes IR (graph, node, types, verifier)
- ✅ Machine IR contracts (the adapter boundary)
- ✅ Register allocator interface
- ✅ Machine emitter interface
- ✅ Code object with relocations, safepoints, deopt points
- ✅ IR verifier with use-def consistency checks
- ✅ Testing framework with 91 passing tests
- ✅ CI configuration (debug, release, ASan+UBSan, Clang)

The interpreter can run programs like:
```js
function fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }
print(fib(10));  // 55

// Closures
function makeCounter() {
    let count = 0;
    return function() { count++; return count; };
}
let c = makeCounter();
print(c(), c(), c());  // 1 2 3

// Try/catch
try { throw "oops"; } catch (e) { print("caught:", e); }

// For-in / for-of
for (let k in {a:1, b:2}) print(k);
for (let v of [10, 20, 30]) print(v);

// Math and string methods
print(Math.sqrt(16), Math.PI);
print("HELLO".toLowerCase());
print(Object.keys({x:1, y:2}));
```

In progress:
- 🚧 Type feedback system
- 🚧 Inline caches
- 🚧 IR builder (bytecode → Sea-of-Nodes)
- 🚧 Optimizer passes
- 🚧 Lowering (IR → Machine IR)
- 🚧 Code emitter adapters (asmjit)
- 🚧 Deoptimization
- 🚧 GC mark-sweep (currently a no-op stub)
- 🚧 Try/finally (finally block semantics)
- 🚧 Class semantics (constructors, super, prototypes)
- 🚧 Switch statements
- 🚧 Template literals
- 🚧 Destructuring

## License

See LICENSE file.
