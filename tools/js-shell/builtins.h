// =============================================================================
// tools/js-shell/builtins.h
// =============================================================================
// Built-in host functions exposed to JS code by the shell.
//
// These are registered on the global object before running user code:
//   - print(...args)         — print values
//   - parseInt(str)          — parse integer
//   - parseFloat(str)        — parse float
//   - isNaN(v)               — true if v is NaN
//   - Array.isArray(v)       — true if v is an array
//   - Object.keys(obj)       — array of property names
//   - Math.{abs,floor,ceil,round,sqrt,pow,min,max,random,PI,E}
//   - String.fromCharCode(n) — char from code point
//   - console.log(...args)   — alias for print

#ifndef V12_TOOLS_JS_SHELL_BUILTINS_H_
#define V12_TOOLS_JS_SHELL_BUILTINS_H_

#include "interpreter/interpreter.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/runtime/runtime.h"

namespace v12 {

// Register all built-in host functions on the global object.
void RegisterBuiltins(Isolate* iso);

}  // namespace v12

#endif  // V12_TOOLS_JS_SHELL_BUILTINS_H_
