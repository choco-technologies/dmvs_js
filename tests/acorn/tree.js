// tree.js SCRIPT.js - acorn's tree of a script as dmvs_js_dump() writes it (see compare.sh)
const acorn = require('acorn');
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
const ast = acorn.parse(src, { ecmaVersion: 2022, sourceType: 'script', allowHashBang: true });
const F = { computed: 1, optional: 2, prefix: 4, shorthand: 8, method: 16, get: 32, set: 64, arrow: 128, expr: 256,
            async: 512, generator: 1024, static: 2048, delegate: 4096, pattern: 8192, decl: 16384, inexact: 32768 };
const NAMES = Object.keys(F);
function flags(f) { let s = ''; for (const k of NAMES) if (f & F[k]) s += ' ' + k; return s; }
function lat(s) { return Buffer.from(s, 'utf8').toString('latin1'); }
function str(s) {
  const b = Buffer.from(s, 'utf8'); let o = '"';
  for (const c of b) {
    if (c === 0x22 || c === 0x5c) o += '\\' + String.fromCharCode(c);
    else if (c === 10) o += '\\n';
    else if (c < 0x20 || c === 0x7f) o += '\\x' + c.toString(16).toUpperCase().padStart(2, '0');
    else o += String.fromCharCode(c);
  }
  return o + '"';
}
function num(raw) {
  raw = raw.replace(/_/g, '');
  let m = /^0[xXoObB]/.test(raw);
  if (m) { const v = BigInt(raw); const max = (1n << 63n) - 1n; return v > max ? max.toString() + '~' : v.toString(); }
  // the lexer's algorithm
  let v = 0n, digits = 0, decimals = 0, inexact = false, fraction = false, i = 0;
  for (; i < raw.length; i++) {
    const d = raw[i];
    if (d === '.' && !fraction) { fraction = true; continue; }
    if (!(d >= '0' && d <= '9')) break;
    if (digits === 0 && d === '0' && !fraction) continue;
    if (digits < 18) { v = v * 10n + BigInt(d); digits++; if (fraction) decimals++; }
    else if (fraction) inexact = inexact || d !== '0';
    else { decimals--; inexact = inexact || d !== '0'; }
  }
  if (raw[i] === 'e' || raw[i] === 'E') { const e = parseInt(raw.slice(i + 1), 10); decimals -= e; }
  const MAX = (1n << 63n) - 1n;
  while (decimals < 0) { if (v > MAX / 10n) { inexact = true; break; } v *= 10n; decimals++; }
  while (decimals > 30 || (decimals > 0 && v % 10n === 0n && v !== 0n)) { inexact = inexact || (v % 10n !== 0n); v /= 10n; decimals--; }
  if (v === 0n) decimals = 0;
  let s = v.toString();
  if (decimals > 0) { s = s.padStart(decimals + 1, '0'); s = s.slice(0, s.length - decimals) + '.' + s.slice(s.length - decimals); }
  return s + (inexact ? '~' : '');
}
function list(a) { return '[' + a.map(d).join(' ') + ']'; }
function node(kind, f, text, slots, lists) {
  let o = '(' + kind + flags(f);
  if (text !== undefined && text !== null) o += ' ' + text;
  let last = -1; slots.forEach((s, i) => { if (s !== null && s !== undefined && !(Array.isArray(s) && s.length === 0)) last = i; });
  for (let i = 0; i <= last; i++) {
    const s = slots[i];
    o += ' ' + (Array.isArray(s) ? list(s) : (s === null || s === undefined) ? '_' : d(s));
  }
  return o + ')';
}
function fn(n, extra) {
  let f = extra | 0;
  if (n.type === 'ArrowFunctionExpression') { f |= F.arrow; if (n.expression) f |= F.expr; }
  if (n.async) f |= F.async;
  if (n.generator) f |= F.generator;
  if (n.type === 'FunctionDeclaration') f |= F.decl;
  return node('function', f, n.id ? str(n.id.name) : null, [n.params, n.body]);
}
function key(k, computed) { if (computed) return d(k); if (k.type === 'PrivateIdentifier') return '#' + k.name; return d(k); }
function d(n) {
  if (n === null) return '<hole>';
  switch (n.type) {
    case 'Program': return node('program', 0, null, [n.body]);
    case 'VariableDeclaration': return '(var ' + n.kind + (n.declarations.length ? ' ' + list(n.declarations) : '') + ')';
    case 'VariableDeclarator': return node('declarator', 0, null, [n.id, n.init]);
    case 'FunctionDeclaration': case 'FunctionExpression': case 'ArrowFunctionExpression': return fn(n);
    case 'ReturnStatement': return node('return', 0, null, [n.argument]);
    case 'IfStatement': return node('if', 0, null, [n.test, n.consequent, n.alternate]);
    case 'ForStatement': return node('for', 0, null, [n.init, n.test, n.update, n.body]);
    case 'ForInStatement': return node('for-in', 0, null, [n.left, n.right, n.body]);
    case 'ForOfStatement': return node('for-of', 0, null, [n.left, n.right, n.body]);
    case 'WhileStatement': return node('while', 0, null, [n.test, n.body]);
    case 'DoWhileStatement': return node('do-while', 0, null, [n.body, n.test]);
    case 'BreakStatement': return node('break', 0, n.label ? str(n.label.name) : null, []);
    case 'ContinueStatement': return node('continue', 0, n.label ? str(n.label.name) : null, []);
    case 'SwitchStatement': return node('switch', 0, null, [n.discriminant, n.cases]);
    case 'SwitchCase': return node('case', 0, null, [n.test, n.consequent]);
    case 'BlockStatement': case 'StaticBlock': return node('block', 0, null, [n.body]);
    case 'ExpressionStatement': return node('expression', 0, null, [n.expression]);
    case 'EmptyStatement': return '(empty)';
    case 'DebuggerStatement': return '(empty)';
    case 'ThrowStatement': return node('throw', 0, null, [n.argument]);
    case 'TryStatement': return node('try', 0, null, [n.block, n.handler ? n.handler.param : null, n.handler ? n.handler.body : null, n.finalizer]);
    case 'LabeledStatement': return node('labeled', 0, str(n.label.name), [n.body]);
    case 'ClassDeclaration': case 'ClassExpression':
      return node('class', n.type === 'ClassDeclaration' ? F.decl : 0, n.id ? str(n.id.name) : null, [n.superClass, n.body.body]);
    case 'MethodDefinition': {
      let f = F.method | (n.static ? F.static : 0) | (n.computed ? F.computed : 0) | (n.kind === 'get' ? F.get : 0) | (n.kind === 'set' ? F.set : 0);
      return '(field' + flags(f) + ' ' + key(n.key, n.computed) + ' ' + fn(n.value, F.method | (n.kind === 'get' ? F.get : 0) | (n.kind === 'set' ? F.set : 0)) + ')';
    }
    case 'PropertyDefinition': {
      let f = (n.static ? F.static : 0) | (n.computed ? F.computed : 0);
      return '(field' + flags(f) + ' ' + key(n.key, n.computed) + (n.value ? ' ' + d(n.value) : '') + ')';
    }
    case 'Identifier': return lat(n.name);
    case 'PrivateIdentifier': return '#' + n.name;
    case 'Literal':
      if (n.regex) return '(regex ' + n.raw + ')';
      if (typeof n.value === 'number') return num(n.raw);
      if (typeof n.value === 'string') return str(n.value);
      if (typeof n.value === 'boolean') return n.value ? 'true' : 'false';
      if (n.value === null) return 'null';
      return '?literal';
    case 'TemplateLiteral': {
      const parts = []; n.quasis.forEach((q, i) => { parts.push({ type: 'Literal', value: q.value.cooked === null ? '' : q.value.cooked }); if (i < n.expressions.length) parts.push(n.expressions[i]); });
      return node('template', 0, null, [parts]);
    }
    case 'TaggedTemplateExpression': {
      const t = n.quasi; const parts = []; t.quasis.forEach((q, i) => { parts.push({ type: 'Literal', value: q.value.cooked === null ? '' : q.value.cooked }); if (i < t.expressions.length) parts.push(t.expressions[i]); });
      return node('template', 0, null, [parts, n.tag]);
    }
    case 'ThisExpression': return 'this';
    case 'Super': return 'super';
    case 'ArrayExpression': return node('array', 0, null, [n.elements]);
    case 'ArrayPattern': return node('array', F.pattern, null, [n.elements]);
    case 'ObjectExpression': return node('object', 0, null, [n.properties]);
    case 'ObjectPattern': return node('object', F.pattern, null, [n.properties]);
    case 'Property': {
      let f = (n.computed ? F.computed : 0) | (n.shorthand ? F.shorthand : 0) | ((n.method || n.kind !== 'init') ? F.method : 0) |
              (n.kind === 'get' ? F.get : 0) | (n.kind === 'set' ? F.set : 0);
      const v = (n.method || n.kind !== 'init') ? fn(n.value, F.method | (n.kind === 'get' ? F.get : 0) | (n.kind === 'set' ? F.set : 0)) : d(n.value);
      return '(property' + flags(f) + ' ' + key(n.key, n.computed) + ' ' + v + ')';
    }
    case 'SpreadElement': case 'RestElement': return node('spread', 0, null, [n.argument]);
    case 'AssignmentPattern': return '(assign = ' + d(n.left) + ' ' + d(n.right) + ')';
    case 'UnaryExpression': return '(unary ' + n.operator + ' ' + d(n.argument) + ')';
    case 'UpdateExpression': return '(update ' + n.operator + (n.prefix ? ' prefix' : '') + ' ' + d(n.argument) + ')';
    case 'BinaryExpression': return '(binary ' + n.operator + ' ' + d(n.left) + ' ' + d(n.right) + ')';
    case 'LogicalExpression': return '(logical ' + n.operator + ' ' + d(n.left) + ' ' + d(n.right) + ')';
    case 'AssignmentExpression': return '(assign ' + n.operator + ' ' + d(n.left) + ' ' + d(n.right) + ')';
    case 'ConditionalExpression': return node('conditional', 0, null, [n.test, n.consequent, n.alternate]);
    case 'CallExpression': return node('call', n.optional ? F.optional : 0, null, [n.callee, n.arguments]);
    case 'NewExpression': return node('new', 0, null, [n.callee, n.arguments]);
    case 'MemberExpression': return node('member', (n.computed ? F.computed : 0) | (n.optional ? F.optional : 0), null, [n.object, n.computed ? n.property : n.property]);
    case 'ChainExpression': return d(n.expression);
    case 'SequenceExpression': return node('sequence', 0, null, [n.expressions]);
    case 'AwaitExpression': return node('await', 0, null, [n.argument]);
    case 'YieldExpression': return node('yield', n.delegate ? F.delegate : 0, null, [n.argument]);
    case 'MetaProperty': return n.meta.name + '.' + n.property.name;
    case 'ParenthesizedExpression': return d(n.expression);
    default: return '?' + n.type;
  }
}
process.stdout.write(Buffer.from(d(ast), 'latin1'));
