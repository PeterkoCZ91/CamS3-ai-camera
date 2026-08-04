#!/usr/bin/env node
// Static checks for the web UI in data/www/. Run from the repository root:
//
//   node tools/check_www.js            # everything
//   node tools/check_www.js --scripts  # syntax of inline <script> blocks
//   node tools/check_www.js --i18n     # translation parity and key coverage
//
// These are the two classes of breakage that a firmware build cannot catch: the UI
// lives in a filesystem image, so a syntax error in an inline script or a missing
// translation key only shows up on the device — as a blank page or as a raw key like
// "settings.motion.enabled" rendered where a label should be.
//
// No dependencies, no network. Exits non-zero on the first category that fails.

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WWW = path.join(__dirname, '..', 'data', 'www');
const HTML = fs.readdirSync(WWW).filter(f => f.endsWith('.html')).sort();
const JS = fs.readdirSync(WWW).filter(f => f.endsWith('.js')).sort();

let failed = false;
const fail = msg => { failed = true; console.error('  FAIL ' + msg); };
const ok = msg => console.log('  ok   ' + msg);

// ── inline <script> syntax ───────────────────────────────────────────────────
// new vm.Script() parses without executing, which is what `node --check` does.
function checkScripts() {
  console.log('Inline script syntax:');
  for (const file of HTML) {
    const src = fs.readFileSync(path.join(WWW, file), 'utf8');
    const blocks = [...src.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)];
    if (blocks.length === 0) { ok(`${file} (no inline script)`); continue; }
    let bad = 0;
    blocks.forEach((m, i) => {
      try {
        new vm.Script(m[1], { filename: `${file}#script${i}` });
      } catch (e) {
        bad++;
        fail(`${file} block ${i}: ${e.message}`);
      }
    });
    if (!bad) ok(`${file} (${blocks.length} block(s))`);
  }
}

// ── translation tables ───────────────────────────────────────────────────────
// i18n.js is an IIFE, so it cannot simply be required. The tables are extracted by
// locating the `en: {` / `cs: {` blocks and reading the quoted keys out of them.
function readTables() {
  const src = fs.readFileSync(path.join(WWW, 'i18n.js'), 'utf8');
  const enAt = src.indexOf('        en: {');
  const csAt = src.indexOf('        cs: {');
  if (enAt < 0 || csAt < 0) throw new Error('cannot locate en/cs tables in i18n.js');
  const endAt = src.indexOf('\n    };', csAt);
  if (endAt < 0) throw new Error('cannot locate end of translation tables');

  const keysOf = block => [...block.matchAll(/^\s+'([A-Za-z0-9_.]+)':/gm)].map(m => m[1]);
  return {
    en: keysOf(src.slice(enAt, csAt)),
    cs: keysOf(src.slice(csAt, endAt)),
  };
}

function checkI18n() {
  console.log('Translations:');
  const { en, cs } = readTables();

  const dup = list => list.filter((k, i) => list.indexOf(k) !== i);
  const dupEn = dup(en), dupCs = dup(cs);
  if (dupEn.length) fail('duplicate keys in en: ' + [...new Set(dupEn)].join(', '));
  if (dupCs.length) fail('duplicate keys in cs: ' + [...new Set(dupCs)].join(', '));

  const setEn = new Set(en), setCs = new Set(cs);
  const onlyEn = [...setEn].filter(k => !setCs.has(k));
  const onlyCs = [...setCs].filter(k => !setEn.has(k));
  if (onlyEn.length) fail('missing Czech translation: ' + onlyEn.join(', '));
  if (onlyCs.length) fail('missing English translation: ' + onlyCs.join(', '));
  if (!onlyEn.length && !onlyCs.length && !dupEn.length && !dupCs.length) {
    ok(`parity ${setEn.size}/${setCs.size} keys`);
  }

  // Every key referenced by markup or page code must exist, or the UI renders the
  // key itself.
  const defined = new Set([...setEn, ...setCs]);
  const missing = new Set();
  const scan = (file, text) => {
    const attr = /data-i18n(?:-placeholder|-title|-aria-label|-alt)?="([^"]+)"/g;
    const call = /\bT\('([^']+)'\)/g;
    for (const m of text.matchAll(attr)) if (!defined.has(m[1])) missing.add(`${file}: ${m[1]}`);
    for (const m of text.matchAll(call)) if (!defined.has(m[1])) missing.add(`${file}: ${m[1]}`);
  };
  for (const f of [...HTML, ...JS]) {
    if (f === 'i18n.js') continue;  // the table itself
    scan(f, fs.readFileSync(path.join(WWW, f), 'utf8'));
  }
  if (missing.size) {
    for (const m of missing) fail('undefined key used — ' + m);
  } else {
    ok('every referenced key is defined');
  }
}

const args = process.argv.slice(2);
const all = args.length === 0;
if (all || args.includes('--scripts')) checkScripts();
if (all || args.includes('--i18n')) checkI18n();

if (failed) {
  console.error('\nweb UI checks FAILED');
  process.exit(1);
}
console.log('\nweb UI checks passed');
