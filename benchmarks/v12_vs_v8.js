// V12 vs V8 (Node.js) Benchmark
// This file runs on BOTH engines. Uses only features V12 supports.

// 1. Arithmetic loop
var s = 0;
for (var i = 0; i < 1000000; i++) { s += i; }
print(s);

// 2. Nested loops (matrix sum)
var sum = 0;
for (var i = 0; i < 100; i++) {
    for (var j = 0; j < 100; j++) {
        sum += i * j;
    }
}
print(sum);

// 3. Fibonacci (recursive)
function fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
print(fib(30));

// 4. Function call overhead
function f(x) { return x + 1; }
var acc = 0;
for (var i = 0; i < 1000000; i++) { acc = f(acc); }
print(acc);

// 5. Object property access
var obj = { x: 0, y: 0, z: 0 };
for (var i = 0; i < 1000000; i++) {
    obj.x = i;
    obj.y = obj.x + 1;
    obj.z = obj.y + 1;
}
print(obj.x, obj.y, obj.z);

// 6. Array operations
var arr = [0, 0, 0, 0, 0];
for (var i = 0; i < 1000000; i++) {
    arr[i % 5] = i;
}
print(arr[0], arr[4]);

// 7. String concatenation
var str = "";
for (var i = 0; i < 10000; i++) { str = str + "x"; }
print(str.length);

// 8. While loop
var w = 0;
var i = 0;
while (i < 1000000) { w += i; i++; }
print(w);

// 9. Conditional logic
var evens = 0;
var odds = 0;
for (var i = 0; i < 1000000; i++) {
    if (i % 2 == 0) { evens++; } else { odds++; }
}
print(evens, odds);

// 10. Closure
function makeAdder(x) {
    return function(y) { return x + y; };
}
var add5 = makeAdder(5);
var closeSum = 0;
for (var i = 0; i < 1000000; i++) { closeSum = add5(closeSum); }
print(closeSum);
