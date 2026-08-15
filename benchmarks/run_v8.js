// Node.js wrapper — replaces `print` with console.log and adds timing
const fs = require('fs');
const { execFileSync } = require('child_process');

const source = fs.readFileSync(__dirname + '/v12_vs_v8.js', 'utf8');

// Replace `print` with `console.log`
const js = source.replace(/print\(/g, 'console.log(');

// Write temp file
fs.writeFileSync('/tmp/bench_v8.js', js);

// Run with Node.js and time it
console.log('=== Node.js (V8) ===');
const t0 = process.hrtime.bigint();
try {
    execFileSync('node', ['/tmp/bench_v8.js'], { stdio: 'inherit', timeout: 60000 });
} catch (e) {
    console.error(e.message);
}
const t1 = process.hrtime.bigint();
const ms = Number(t1 - t0) / 1e6;
console.log(`\nTotal: ${ms.toFixed(0)} ms`);
