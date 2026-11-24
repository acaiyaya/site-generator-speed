import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
let native;
let fallback;
try {
  const candidate = require('./sitegen.node').run;
  if (typeof candidate !== 'function') throw new TypeError('Missing native run export');
  native = candidate;
} catch {
  fallback = (await import('./js-solve.js')).run;
}

export function run(input) {
  if (!native) return fallback(input);
  try {
    return native(input);
  } catch (error) {
    if (!error?.message?.startsWith('SITEGE_FALLBACK:')) throw error;
    // The driver requires a synchronous result, including on the first retry.
    fallback ??= require('./js-solve.js').run;
    return fallback(input);
  }
}
