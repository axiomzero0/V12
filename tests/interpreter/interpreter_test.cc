// =============================================================================
// tests/interpreter/interpreter_test.cc
// =============================================================================
// End-to-end tests for the bytecode generator + register interpreter.
//
// Each test compiles a small JS snippet, runs it through the interpreter,
// and checks the output (via a captured `print` host function).

#include "tests/test-framework.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/arena.h"
#include "frontend/bytecode/bytecode-generator.h"
#include "frontend/parser/parser.h"
#include "interpreter/interpreter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

using namespace v12;
using namespace v12_test;

namespace {

// Thread-local pointer to the current test harness's output vector.
// This lets the `print` host function (which has a C function signature
// and can't capture a closure) forward output to the right harness.
thread_local std::vector<std::string>* t_output = nullptr;

Value TestPrintHost(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    std::string line;
    for (uint32_t i = 0; i < argc; ++i) {
        if (i > 0) line += " ";
        Value s = ToString(iso, args[i]);
        if (s.IsString()) {
            line += std::string(static_cast<JSString*>(s.AsHeapObject())->view());
        }
    }
    if (t_output != nullptr) {
        t_output->push_back(line);
    }
    return iso->undefined_value();
}

// Test harness: compiles and runs a JS snippet, capturing `print` output.
class InterpHarness {
public:
    InterpHarness() : iso_(), saved_output_(t_output) {
        HostFunction* print_fn = HostFunction::New(&iso_, &TestPrintHost, 0);
        iso_.SetGlobal("print", Value::FromHeap(print_fn));
        t_output = &output_;
    }
    ~InterpHarness() { t_output = saved_output_; }

    InterpResult Run(std::string_view source) {
        Arena arena;
        Parser parser(&arena, source);
        Program* prog = parser.ParseProgram();
        if (parser.has_error()) {
            last_error_ = parser.errors().empty() ? "unknown" : parser.errors()[0].message;
            return {InterpStatus::kThrew, Value::FromHeap(JSString::New(&iso_, last_error_))};
        }
        BytecodeGenerator gen(&iso_, &arena);
        auto program = gen.Compile(prog);
        Interp interp(&iso_);
        return interp.Run(program.get());
    }

    std::vector<std::string>& output() { return output_; }
    const std::string& last_error() const { return last_error_; }

private:
    Isolate iso_;
    std::vector<std::string> output_;
    std::string last_error_;
    std::vector<std::string>* saved_output_;
};

}  // namespace

// =============================================================================
// Arithmetic
// =============================================================================
TEST(Interpreter, Arithmetic) {
    InterpHarness h;
    h.Run("print(1 + 2); print(10 - 3); print(4 * 5); print(20 / 4); print(17 % 5);");
    ASSERT_EQ(h.output().size(), 5u);
    EXPECT_EQ(h.output()[0], "3");
    EXPECT_EQ(h.output()[1], "7");
    EXPECT_EQ(h.output()[2], "20");
    EXPECT_EQ(h.output()[3], "5");
    EXPECT_EQ(h.output()[4], "2");
}

TEST(Interpreter, Negation) {
    InterpHarness h;
    h.Run("print(-5); print(-(-5));");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "-5");
    EXPECT_EQ(h.output()[1], "5");
}

// =============================================================================
// Variables
// =============================================================================
TEST(Interpreter, Variables) {
    InterpHarness h;
    h.Run("let x = 10; let y = 20; print(x + y);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "30");
}

TEST(Interpreter, VarAndConst) {
    InterpHarness h;
    h.Run("var a = 1; const b = 2; let c = 3; print(a + b + c);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "6");
}

// =============================================================================
// Strings
// =============================================================================
TEST(Interpreter, StringConcat) {
    InterpHarness h;
    h.Run("print(\"hello\" + \" \" + \"world\");");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "hello world");
}

TEST(Interpreter, StringLength) {
    InterpHarness h;
    h.Run("print(\"hello\".length);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "5");
}

TEST(Interpreter, StringIndex) {
    InterpHarness h;
    h.Run("print(\"abc\"[1]);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "b");
}

// =============================================================================
// Functions
// =============================================================================
TEST(Interpreter, FunctionCall) {
    InterpHarness h;
    h.Run("function add(a, b) { return a + b; } print(add(3, 4));");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "7");
}

TEST(Interpreter, Recursion) {
    InterpHarness h;
    h.Run("function fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2); } print(fib(10));");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "55");
}

TEST(Interpreter, NestedFunction) {
    InterpHarness h;
    h.Run("function outer() { function inner() { return 42; } return inner(); } print(outer());");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "42");
}

TEST(Interpreter, EarlyReturn) {
    InterpHarness h;
    h.Run("function f(x) { if (x > 0) return \"pos\"; return \"neg\"; } print(f(5)); print(f(-1));");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "pos");
    EXPECT_EQ(h.output()[1], "neg");
}

// =============================================================================
// Objects
// =============================================================================
TEST(Interpreter, ObjectLiteral) {
    InterpHarness h;
    h.Run("let obj = { name: \"Alice\", age: 30 }; print(obj.name, obj.age);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "Alice 30");
}

TEST(Interpreter, ObjectMutation) {
    InterpHarness h;
    h.Run("let obj = {}; obj.x = 42; print(obj.x);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "42");
}

TEST(Interpreter, ObjectPropertyAdd) {
    InterpHarness h;
    h.Run("let obj = { a: 1 }; obj.b = 2; obj.c = 3; print(obj.a, obj.b, obj.c);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "1 2 3");
}

// =============================================================================
// Arrays
// =============================================================================
TEST(Interpreter, ArrayLiteral) {
    InterpHarness h;
    h.Run("let arr = [1, 2, 3, 4, 5]; print(arr[0], arr[4]);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "1 5");
}

TEST(Interpreter, ArrayLength) {
    InterpHarness h;
    h.Run("let arr = [1, 2, 3]; print(arr.length);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "3");
}

TEST(Interpreter, ArrayPush) {
    InterpHarness h;
    h.Run("let arr = [1, 2]; arr.push(3); print(arr.length); print(arr[2]);");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "3");
    EXPECT_EQ(h.output()[1], "3");
}

// =============================================================================
// Control flow
// =============================================================================
TEST(Interpreter, ForLoop) {
    InterpHarness h;
    h.Run("let sum = 0; for (let i = 1; i <= 10; i++) { sum += i; } print(sum);");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "55");
}

TEST(Interpreter, WhileLoop) {
    InterpHarness h;
    h.Run("let i = 0; while (i < 3) { print(i); i++; }");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "0");
    EXPECT_EQ(h.output()[1], "1");
    EXPECT_EQ(h.output()[2], "2");
}

TEST(Interpreter, IfElse) {
    InterpHarness h;
    h.Run("let n = 5; if (n > 3) { print(\"big\"); } else { print(\"small\"); }");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "big");
}

TEST(Interpreter, Break) {
    InterpHarness h;
    h.Run("for (let i = 0; i < 10; i++) { if (i == 3) break; print(i); }");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "0");
    EXPECT_EQ(h.output()[1], "1");
    EXPECT_EQ(h.output()[2], "2");
}

TEST(Interpreter, Continue) {
    InterpHarness h;
    h.Run("for (let i = 0; i < 5; i++) { if (i == 2) continue; print(i); }");
    ASSERT_EQ(h.output().size(), 4u);
    EXPECT_EQ(h.output()[0], "0");
    EXPECT_EQ(h.output()[1], "1");
    EXPECT_EQ(h.output()[2], "3");
    EXPECT_EQ(h.output()[3], "4");
}

// =============================================================================
// Operators
// =============================================================================
TEST(Interpreter, Comparison) {
    InterpHarness h;
    h.Run("print(3 < 5); print(5 > 3); print(3 == 3); print(3 === \"3\");");
    ASSERT_EQ(h.output().size(), 4u);
    EXPECT_EQ(h.output()[0], "true");
    EXPECT_EQ(h.output()[1], "true");
    EXPECT_EQ(h.output()[2], "true");
    EXPECT_EQ(h.output()[3], "false");
}

TEST(Interpreter, LogicalOps) {
    InterpHarness h;
    h.Run("print(true && false); print(true || false); print(!true);");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "false");
    EXPECT_EQ(h.output()[1], "true");
    EXPECT_EQ(h.output()[2], "false");
}

TEST(Interpreter, BitwiseOps) {
    InterpHarness h;
    h.Run("print(5 | 3); print(5 & 3); print(5 ^ 3); print(1 << 4);");
    ASSERT_EQ(h.output().size(), 4u);
    EXPECT_EQ(h.output()[0], "7");
    EXPECT_EQ(h.output()[1], "1");
    EXPECT_EQ(h.output()[2], "6");
    EXPECT_EQ(h.output()[3], "16");
}

TEST(Interpreter, Typeof) {
    InterpHarness h;
    h.Run("print(typeof 1); print(typeof \"x\"); print(typeof true); print(typeof undefined); print(typeof null);");
    ASSERT_EQ(h.output().size(), 5u);
    EXPECT_EQ(h.output()[0], "number");
    EXPECT_EQ(h.output()[1], "string");
    EXPECT_EQ(h.output()[2], "boolean");
    EXPECT_EQ(h.output()[3], "undefined");
    EXPECT_EQ(h.output()[4], "object");
}

TEST(Interpreter, CompoundAssignment) {
    InterpHarness h;
    h.Run("let x = 10; x += 5; print(x); x -= 3; print(x); x *= 2; print(x);");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "15");
    EXPECT_EQ(h.output()[1], "12");
    EXPECT_EQ(h.output()[2], "24");
}

TEST(Interpreter, PostfixIncrement) {
    InterpHarness h;
    h.Run("let x = 5; print(x++); print(x);");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "5");
    EXPECT_EQ(h.output()[1], "6");
}

TEST(Interpreter, PrefixIncrement) {
    InterpHarness h;
    h.Run("let x = 5; print(++x); print(x);");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "6");
    EXPECT_EQ(h.output()[1], "6");
}

// =============================================================================
// Closures
// =============================================================================
TEST(Interpreter, ClosureCounter) {
    InterpHarness h;
    h.Run("function makeCounter() { let count = 0; return function() { count++; return count; }; } "
          "let c = makeCounter(); print(c()); print(c()); print(c());");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "1");
    EXPECT_EQ(h.output()[1], "2");
    EXPECT_EQ(h.output()[2], "3");
}

TEST(Interpreter, ClosureCaptureParam) {
    InterpHarness h;
    h.Run("function adder(x) { return function(y) { return x + y; }; } "
          "let add5 = adder(5); print(add5(3)); print(add5(10));");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "8");
    EXPECT_EQ(h.output()[1], "15");
}

TEST(Interpreter, ClosureMultipleCaptures) {
    InterpHarness h;
    h.Run("function makePair(a, b) { return function() { return a + b; }; } "
          "print(makePair(10, 20)());");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "30");
}

TEST(Interpreter, ClosureSharedState) {
    InterpHarness h;
    h.Run("function makeAcc() { let total = 0; "
          "  function add(x) { total += x; return total; } "
          "  return add; } "
          "let acc = makeAcc(); "
          "print(acc(10)); print(acc(20)); print(acc(30));");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "10");
    EXPECT_EQ(h.output()[1], "30");
    EXPECT_EQ(h.output()[2], "60");
}

// =============================================================================
// Try/Catch
// =============================================================================
TEST(Interpreter, TryCatchString) {
    InterpHarness h;
    h.Run("try { throw \"oops\"; } catch (e) { print(\"caught:\", e); }");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "caught: oops");
}

TEST(Interpreter, TryCatchNumber) {
    InterpHarness h;
    h.Run("try { throw 42; } catch (e) { print(\"caught:\", e); }");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "caught: 42");
}

TEST(Interpreter, TryCatchNoThrow) {
    InterpHarness h;
    h.Run("try { print(\"in try\"); } catch (e) { print(\"should not catch\"); } print(\"after\");");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "in try");
    EXPECT_EQ(h.output()[1], "after");
}

TEST(Interpreter, TryCatchNested) {
    InterpHarness h;
    h.Run("try { throw \"inner\"; } catch (e) { print(e); "
          "  try { throw \"nested\"; } catch (e2) { print(e2); } }");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "inner");
    EXPECT_EQ(h.output()[1], "nested");
}

TEST(Interpreter, TryCatchFromFunction) {
    InterpHarness h;
    h.Run("function risky() { throw \"from function\"; } "
          "try { risky(); } catch (e) { print(\"caught:\", e); }");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "caught: from function");
}

TEST(Interpreter, TryCatchNoParam) {
    InterpHarness h;
    h.Run("try { throw 123; } catch { print(\"caught no param\"); }");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "caught no param");
}

// =============================================================================
// For-in
// =============================================================================
TEST(Interpreter, ForInObject) {
    InterpHarness h;
    h.Run("let obj = { a: 1, b: 2, c: 3 }; "
          "for (let k in obj) { print(k, obj[k]); }");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "a 1");
    EXPECT_EQ(h.output()[1], "b 2");
    EXPECT_EQ(h.output()[2], "c 3");
}

// =============================================================================
// For-of
// =============================================================================
TEST(Interpreter, ForOfArray) {
    InterpHarness h;
    h.Run("let arr = [10, 20, 30]; for (let v of arr) { print(v); }");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "10");
    EXPECT_EQ(h.output()[1], "20");
    EXPECT_EQ(h.output()[2], "30");
}

TEST(Interpreter, ForOfString) {
    InterpHarness h;
    h.Run("for (let c of \"abc\") { print(c); }");
    ASSERT_EQ(h.output().size(), 3u);
    EXPECT_EQ(h.output()[0], "a");
    EXPECT_EQ(h.output()[1], "b");
    EXPECT_EQ(h.output()[2], "c");
}

// =============================================================================
// String methods
// =============================================================================
TEST(Interpreter, StringToUpperCase) {
    InterpHarness h;
    h.Run("print(\"hello\".toUpperCase());");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "HELLO");
}

TEST(Interpreter, StringToLowerCase) {
    InterpHarness h;
    h.Run("print(\"WORLD\".toLowerCase());");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "world");
}

TEST(Interpreter, StringSubstring) {
    InterpHarness h;
    h.Run("print(\"hello world\".substring(0, 5));");
    ASSERT_EQ(h.output().size(), 1u);
    EXPECT_EQ(h.output()[0], "hello");
}

TEST(Interpreter, StringCharAt) {
    InterpHarness h;
    h.Run("print(\"abc\".charAt(1)); print(\"abc\".charAt(10));");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "b");
    EXPECT_EQ(h.output()[1], "");
}

TEST(Interpreter, StringIndexOf) {
    InterpHarness h;
    h.Run("print(\"hello world\".indexOf(\"world\")); print(\"hello\".indexOf(\"xyz\"));");
    ASSERT_EQ(h.output().size(), 2u);
    EXPECT_EQ(h.output()[0], "6");
    EXPECT_EQ(h.output()[1], "-1");
}
