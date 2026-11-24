const compare = (a, b) => a < b ? -1 : a > b ? 1 : 0;
const escapes = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const needsEscape = /[&<>"']/;
const escapePattern = /[&<>"']/g;
const escapeCharacter = (character) => escapes[character];
const html = (text) => needsEscape.test(text) ? text.replace(escapePattern, escapeCharacter) : text;
const horizontal = (code) => code === 32 || code === 9;

function trimStart(text, start, end) {
  while (start < end && horizontal(text.charCodeAt(start))) start++;
  return start;
}

function trimEnd(text, start, end) {
  while (end > start && horizontal(text.charCodeAt(end - 1))) end--;
  return end;
}

const nonAscii = /[^\x00-\x7f]/;
const marks = /\p{M}/gu;
const slugSeparators = /[^A-Za-z0-9]+/g;

function slugOf(text, cache) {
  const cached = cache.get(text);
  if (cached !== undefined) return cached;
  const plain = nonAscii.test(text) ? text.normalize('NFKD').replace(marks, '') : text;
  const joined = plain.replace(slugSeparators, '-');
  const start = joined.charCodeAt(0) === 45 ? 1 : 0;
  let end = Math.min(joined.length, start + 64);
  if (joined.charCodeAt(end - 1) === 45) end--;
  // Only ASCII letters remain, so lowercasing cannot introduce new slug characters.
  const slug = joined.slice(start, end).toLowerCase() || 'untitled';
  cache.set(text, slug);
  return slug;
}

function claimSlug(slug, used) {
  const next = used.get(slug);
  if (next === undefined) {
    used.set(slug, 2);
    return slug;
  }
  let suffix = next;
  let candidate;
  do {
    candidate = slug + '-' + suffix++;
  } while (used.has(candidate));
  used.set(slug, suffix);
  used.set(candidate, 2);
  return candidate;
}

const fieldPattern = /^([a-z_]+):(?: (.*))?$/;
const datePattern = /^(\d{4})-(\d{2})-(\d{2})$/;
const layoutPattern = /^[a-z0-9-]+$/;
const slugPattern = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const invalidTag = /["[\]]/;
const fields = new Set(['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary']);
const monthNames = [
  'January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December',
];
const monthLengths = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

function validDate(raw) {
  const match = datePattern.exec(raw);
  if (!match) return false;
  const year = +match[1], month = +match[2], day = +match[3];
  const leap = month === 2 && year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
  return year >= 1970 && year <= 2099 && day >= 1 && day <= monthLengths[month - 1] + Number(leap);
}

function readString(data, key, raw) {
  if (raw[0] === '[') return 'a bare string opens on [';
  if (raw[0] !== '"') {
    data[key] = raw;
    return;
  }
  const end = raw.length - 1;
  if (end === 0 || raw[end] !== '"') return 'a quoted string is not closed';
  const parts = [];
  let start = 1;
  for (let i = start; i < end; i++) {
    if (raw[i] === '"') return 'a quoted string holds a stray quote';
    if (raw[i] === '\\') {
      const next = raw[i + 1];
      if (i + 1 === end || (next !== '"' && next !== '\\')) {
        return 'a quoted string holds an escape other than \\" or \\\\';
      }
      parts.push(raw.slice(start, i), next);
      start = ++i + 1;
    }
  }
  parts.push(raw.slice(start, end));
  data[key] = parts.length === 1 ? parts[0] : parts.join('');
}

function readField(data, key, raw) {
  switch (key) {
    case 'title':
    case 'summary':
      return readString(data, key, raw);
    case 'date':
      if (!validDate(raw)) return 'not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)';
      break;
    case 'tags': {
      if (raw[0] !== '[' || !raw.endsWith(']')) return 'not a list [a, b]';
      const inner = raw.slice(1, -1);
      const tags = inner.trim() === '' ? [] : inner.split(',');
      for (let i = 0; i < tags.length; i++) {
        const name = tags[i].trim();
        if (name === '' || invalidTag.test(name)) return 'an empty tag, or one holding " [ or ]';
        tags[i] = name;
      }
      data.tags = tags;
      return;
    }
    case 'draft':
      if (raw !== 'true' && raw !== 'false') return 'true or false';
      data.draft = raw === 'true';
      return;
    case 'layout':
      if (!layoutPattern.test(raw)) return 'not a template name';
      break;
    case 'slug':
      if (raw.length > 64 || !slugPattern.test(raw)) return 'not a slug';
      break;
  }
  data[key] = raw;
}

function readFrontMatter(text) {
  if (!text.startsWith('---\n')) return text === '---' ? 'the front matter is not closed' : 'no front matter';
  let close = text.indexOf('\n---\n', 3);
  if (close < 0) {
    if (!text.endsWith('\n---')) return 'the front matter is not closed';
    close = text.length - 4;
  }

  const data = {};
  for (let start = 4, number = 2; start < close; number++) {
    const end = text.indexOf('\n', start);
    if (text.charCodeAt(start) !== 35 && trimStart(text, start, end) !== end) {
      const line = text.slice(start, end);
      const match = fieldPattern.exec(line);
      if (!match) return `line ${number} is not "key: value"`;
      const key = match[1];
      if (!fields.has(key)) return `unknown key "${key}"`;
      if (Object.hasOwn(data, key)) return `key "${key}" is given twice`;
      const raw = match[2] ?? '';
      const value = raw.slice(0, trimEnd(raw, 0, raw.length));
      if (value === '') return `key "${key}" has no value`;
      const error = readField(data, key, value);
      if (error !== undefined) return `key "${key}": ${error}`;
    }
    start = end + 1;
  }
  return data.title === undefined ? 'no title' : { data, body: text.slice(close + 5) };
}

// Template instructions share one output buffer, including when entering loops and includes.
const TEXT = 0, ESCAPED = 1, RAW = 2, TAG = 3, LOOP = 4, IF = 5, UNLESS = 6, JUMP = 7, INCLUDE = 8;
const pathPattern = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const forPattern = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const ifPattern = /^if (not )?(\S+)$/;
const includePattern = /^include "([a-z0-9-]+)"$/;

function makeInstruction(op, value, binding) {
  return { op, value, end: 0, binding };
}

function compile(source) {
  const instructions = [];
  let literal = 0;
  let search = 0;
  for (;;) {
    const open = source.indexOf('{', search);
    if (open < 0) break;
    const next = source[open + 1];
    if (next !== '{' && next !== '%') {
      search = open + 1;
      continue;
    }
    const tag = next === '%';
    const raw = !tag && source[open + 2] === '{';
    const width = raw ? 3 : 2;
    const close = source.indexOf(tag ? '%}' : raw ? '}}}' : '}}', open + width);
    if (close < 0) return `a ${source.slice(open, open + width)} is not closed`;
    let literalEnd = open;
    search = close + width;
    if (tag) {
      const lineStart = source.lastIndexOf('\n', open - 1) + 1;
      const newline = source.indexOf('\n', search);
      const lineEnd = newline < 0 ? source.length : newline;
      if (trimStart(source, lineStart, open) === open && trimStart(source, search, lineEnd) === lineEnd) {
        literalEnd = lineStart;
        search = newline < 0 ? lineEnd : lineEnd + 1;
      }
    }
    if (literalEnd > literal) instructions.push(makeInstruction(TEXT, source.slice(literal, literalEnd)));
    const token = source.slice(open + width, close).trim();
    if (!tag && !pathPattern.test(token)) return `${JSON.stringify(token)} is not a path`;
    instructions.push(tag ? makeInstruction(TAG, token) : makeInstruction(raw ? RAW : ESCAPED, token.split('.')));
    literal = search;
  }
  if (literal < source.length) instructions.push(makeInstruction(TEXT, source.slice(literal)));

  // Validate interpolation tokens before reporting any errors in the control structure.
  const code = [];
  const openBlocks = [];
  for (const instruction of instructions) {
    if (instruction.op !== TAG) {
      code.push(instruction);
      continue;
    }
    const token = instruction.value;
    const top = openBlocks[openBlocks.length - 1];
    const loop = forPattern.exec(token);
    const branch = loop ? null : ifPattern.exec(token);
    if (loop || branch) {
      const path = loop ? loop[2] : branch[2];
      if (!pathPattern.test(path)) return `${JSON.stringify(path)} is not a path`;
      const block = makeInstruction(loop ? LOOP : branch[1] ? UNLESS : IF, path.split('.'), loop?.[1]);
      code.push(block);
      openBlocks.push({ block, skip: null });
    } else if (token === 'else') {
      if (!top || top.block.op === LOOP || top.skip) return 'an else outside an if';
      const skip = makeInstruction(JUMP);
      code.push(skip);
      top.block.end = code.length;
      top.skip = skip;
    } else if (token === 'endfor' || token === 'endif') {
      if (!top || (top.block.op === LOOP) !== (token === 'endfor')) {
        return `an ${token} with no open ${token.slice(3)}`;
      }
      (top.skip ?? top.block).end = code.length;
      openBlocks.pop();
    } else {
      const include = includePattern.exec(token);
      if (!include) return `{% ${token} %} is not a tag`;
      code.push(makeInstruction(INCLUDE, include[1]));
    }
  }
  const openBlock = openBlocks.at(-1);
  return openBlock ? `a ${openBlock.block.op === LOOP ? 'for' : 'if'} is not closed` : code;
}

function lookup(root, frame, path) {
  const name = path[0];
  let value;
  if (name === 'loop' && frame !== null) {
    value = frame.loop;
  } else {
    while (frame !== null && frame.name !== name) frame = frame.parent;
    value = frame !== null ? frame.value : Object.hasOwn(root, name) ? root[name] : undefined;
  }
  for (let i = 1; i < path.length; i++) {
    if (value === null || typeof value !== 'object' || !Object.hasOwn(value, path[i])) return;
    value = value[path[i]];
  }
  return value;
}

function asText(value) {
  switch (typeof value) {
    case 'string': return value;
    case 'number': return String(value);
    case 'boolean': return value ? 'true' : 'false';
    default: return Array.isArray(value) ? value.map(asText).join(', ') : '';
  }
}

function isTrue(value) {
  return Array.isArray(value) ? value.length > 0 : !!value || Number.isNaN(value);
}

function emit(code, start, end, root, frame, depth, out) {
  for (let i = start; i < end;) {
    const instruction = code[i++];
    const { op, value } = instruction;
    switch (op) {
      case TEXT:
        out.push(value);
        break;
      case ESCAPED:
        out.push(html(asText(lookup(root, frame, value))));
        break;
      case RAW:
        out.push(asText(lookup(root, frame, value)));
        break;
      case IF:
      case UNLESS:
        if (isTrue(lookup(root, frame, value)) === (op === UNLESS)) i = instruction.end;
        break;
      case JUMP:
        i = instruction.end;
        break;
      case LOOP: {
        const values = lookup(root, frame, value);
        if (Array.isArray(values) && values.length) {
          const length = values.length;
          const loop = { index: 0, first: true, last: false, length };
          const child = { name: instruction.binding, value: undefined, loop, parent: frame };
          for (let k = 0; k < length; k++) {
            child.value = values[k];
            loop.index = k + 1;
            loop.first = k === 0;
            loop.last = loop.index === length;
            emit(code, i, instruction.end, root, child, depth, out);
          }
        }
        i = instruction.end;
        break;
      }
      case INCLUDE:
        if (value && depth < 8) emit(value, 0, value.length, root, frame, depth + 1, out);
        break;
    }
  }
}

const inlineSpecial = /[\\`*[&<>"']/g;
const escapable = new Set('\\`*_[]()#+-.!>');
const whitespace = /\s/;

function isEscaped(text, at, from) {
  let before = at;
  while (before > from && text.charCodeAt(before - 1) === 92) before--;
  return ((at - before) & 1) !== 0;
}

function delimiter(text, marker, from, singleStar = false) {
  let at = text.indexOf(marker, from);
  while (at !== -1) {
    if (isEscaped(text, at, from)) at = text.indexOf(marker, at + 1);
    else if (singleStar && text.charCodeAt(at + 1) === 42) at = text.indexOf(marker, at + 2);
    else return at;
  }
  return -1;
}

function inline(text, link) {
  inlineSpecial.lastIndex = 0;
  if (!inlineSpecial.test(text)) return text;
  const parts = [];
  writeInline(text, link, parts, inlineSpecial.lastIndex);
  return parts.join('');
}

function writeNestedInline(text, link, parts) {
  inlineSpecial.lastIndex = 0;
  if (inlineSpecial.test(text)) writeInline(text, link, parts, inlineSpecial.lastIndex);
  else parts.push(text);
}

function writeInline(text, link, parts, next) {
  let start = 0;
  do {
    const at = next - 1;
    const character = text[at];
    if (at > start) parts.push(text.slice(start, at));
    start = next;
    switch (character) {
      case '\\': {
        const next = text[start];
        if (escapable.has(next)) {
          parts.push(escapes[next] ?? next);
          start++;
        } else parts.push(character);
        break;
      }
      case '`': {
        const close = text.indexOf('`', start);
        if (close > start) {
          parts.push('<code>', html(text.slice(start, close)), '</code>');
          start = close + 1;
        } else parts.push(character);
        break;
      }
      case '*': {
        const strong = text.charCodeAt(start) === 42;
        if (strong) start++;
        const close = delimiter(text, strong ? '**' : '*', start, !strong);
        if (close > start) {
          parts.push(strong ? '<strong>' : '<em>');
          writeNestedInline(text.slice(start, close), link, parts);
          parts.push(strong ? '</strong>' : '</em>');
          start = close + (strong ? 2 : 1);
        } else parts.push(strong ? '**' : '*');
        break;
      }
      case '[': {
        const labelEnd = delimiter(text, ']', start);
        const close = labelEnd >= 0 && text.charCodeAt(labelEnd + 1) === 40 ? text.indexOf(')', labelEnd + 2) : -1;
        if (close > labelEnd + 2) {
          const target = text.slice(labelEnd + 2, close);
          if (!whitespace.test(target)) {
            parts.push('<a href="', html(link(target)), '">');
            writeNestedInline(text.slice(start, labelEnd), link, parts);
            parts.push('</a>');
            start = close + 1;
            break;
          }
        }
        parts.push(character);
        break;
      }
      default:
        parts.push(escapes[character]);
    }
    inlineSpecial.lastIndex = start;
  } while (inlineSpecial.test(text) && (next = inlineSpecial.lastIndex));
  if (start < text.length) parts.push(text.slice(start));
}

const fencePattern = /^```([A-Za-z0-9_+-]*)[ \t]*$/;
const headingPattern = /^(#{1,6})[ \t]+(.*?)[ \t]*$/;
const bulletPattern = /^[-*] (.*)$/;
const orderedPattern = /^(\d{1,9})\. (.*)$/;
const continuePattern = /^ {2,}\S/;
const BLANK = 1, FENCE = 2, HEADING = 3, RULE = 4;

function markdown(text, slugs, link) {
  const blocks = [];
  const toc = [];
  const ids = new Map();
  const lines = [];
  const length = text.length;
  let start = 0;
  let end = text.indexOf('\n');
  if (end < 0) end = length;
  let detail;
  let excerpt;

  function advance() {
    start = end + 1;
    end = text.indexOf('\n', start);
    if (end < 0) end = length;
  }

  function boundary() {
    if (start === end) return BLANK;
    const first = text.charCodeAt(start);
    if (horizontal(first) && trimStart(text, start, end) === end) return BLANK;
    if (first === 96 && text.startsWith('```', start)) {
      detail = fencePattern.exec(text.slice(start, end));
      if (detail) return FENCE;
    } else if (first === 35) {
      detail = headingPattern.exec(text.slice(start, end));
      if (detail && detail[2]) return HEADING;
    } else if ((first === 45 && text.startsWith('---', start)) ||
               (first === 42 && text.startsWith('***', start))) {
      if (trimStart(text, start + 3, end) === end) return RULE;
    }
    return 0;
  }

  function takeLine(offset = 0) {
    const from = trimStart(text, start + offset, end);
    const line = text.slice(from, trimEnd(text, from, end));
    advance();
    return line;
  }

  let kind = boundary();
  while (start <= length) {
    if (kind === BLANK) {
      advance();
    } else if (kind === FENCE) {
      const language = detail[1];
      advance();
      const codeStart = start;
      while (start <= length &&
             !(text.startsWith('```', start) && trimStart(text, start + 3, end) === end)) advance();
      const code = start <= length ? text.slice(codeStart, start)
        : codeStart <= length ? text.slice(codeStart) + '\n' : '';
      blocks.push(language ? `<pre><code class="language-${language}">` : '<pre><code>', html(code), '</code></pre>\n');
      if (start <= length) advance();
    } else if (kind === HEADING) {
      const level = detail[1].length;
      const title = detail[2];
      const id = claimSlug(slugOf(title, slugs), ids);
      const rendered = inline(title, link);
      blocks.push(`<h${level} id="${id}">${rendered}</h${level}>\n`);
      if (level === 2 || level === 3) toc.push({ level, id, html: rendered });
      advance();
    } else if (kind === RULE) {
      blocks.push('<hr>\n');
      advance();
    } else if (text.charCodeAt(start) === 62) {
      do {
        const line = takeLine(1);
        if (line) lines.push(line);
      } while (text.charCodeAt(start) === 62);
      blocks.push('<blockquote>\n');
      if (lines.length) blocks.push('<p>', inline(lines.join('\n'), link), '</p>\n');
      blocks.push('</blockquote>\n');
      lines.length = 0;
    } else {
      const first = text.charCodeAt(start);
      const bullet = (first === 45 || first === 42) && text.charCodeAt(start + 1) === 32;
      const pattern = bullet ? bulletPattern : first >= 48 && first <= 57 ? orderedPattern : null;
      let item = pattern && pattern.exec(text.slice(start, end));
      if (item) {
        const firstNumber = bullet ? 1 : +item[1];
        blocks.push(bullet ? '<ul>\n' : firstNumber === 1 ? '<ol>\n' : `<ol start="${firstNumber}">\n`);
        do {
          lines.push(takeLine(bullet ? 2 : item[1].length + 2));
          while (start <= length && text.charCodeAt(start) === 32 && text.charCodeAt(start + 1) === 32 &&
                 continuePattern.test(text.slice(start, end))) lines.push(takeLine());
          blocks.push('<li>', inline(lines.length === 1 ? lines[0] : lines.join('\n'), link), '</li>\n');
          lines.length = 0;
          item = start <= length && pattern.exec(text.slice(start, end));
        } while (item);
        blocks.push(bullet ? '</ul>\n' : '</ol>\n');
      } else {
        do {
          lines.push(takeLine());
          kind = start <= length ? boundary() : BLANK;
        } while (kind === 0);
        const paragraph = inline(lines.length === 1 ? lines[0] : lines.join('\n'), link);
        if (excerpt === undefined) excerpt = paragraph;
        blocks.push('<p>', paragraph, '</p>\n');
        lines.length = 0;
        continue;
      }
    }
    if (start <= length) kind = boundary();
  }
  return { content: blocks.join(''), toc, excerpt: excerpt ?? '' };
}

const scheme = /^[A-Za-z][A-Za-z0-9+.-]*:/;
const lineEndings = /\r\n?/g;
const wordBatch = /\s*\S+(?:\s+\S+){0,199}/y;

function readingMinutes(body) {
  // A greedy match consumes up to 200 words, so only the final match can be short.
  let count = 0;
  wordBatch.lastIndex = 0;
  while (wordBatch.test(body)) count++;
  return Math.max(1, count);
}

function sourceLink(path, dir, base, sources, staticPaths, broken) {
  return (target) => {
    if (target[0] === '#' || target[0] === '/' || scheme.test(target)) return target;
    const hash = target.indexOf('#');
    const rel = hash < 0 ? target : target.slice(0, hash);
    const parts = dir === '' ? [] : dir.split('/');
    for (const segment of rel.split('/')) {
      if (segment === '.' || segment === '') continue;
      if (segment === '..') {
        if (parts.length === 0) {
          broken.add(`broken-link ${path} -> ${target}`);
          return target;
        }
        parts.pop();
      } else parts.push(segment);
    }
    const resolved = parts.join('/');
    const source = sources.get(resolved);
    const fragment = hash < 0 ? '' : target.slice(hash);
    if (source) return source.url + fragment;
    if (staticPaths.has(resolved)) return base + '/' + resolved + fragment;
    broken.add(`broken-link ${path} -> ${target}`);
    return target;
  };
}

function tagReferences(names, cache, byTag, post, base) {
  const refs = [];
  if (!names?.length) return refs;
  const seen = new Set();
  for (const name of names) {
    const slug = slugOf(name, cache);
    if (seen.has(slug)) continue;
    seen.add(slug);
    let group = byTag.get(slug);
    if (!group && post) {
      group = { info: { name, slug, url: `${base}/tags/${slug}/` }, posts: [] };
      byTag.set(slug, group);
    }
    if (post) group.posts.push(post);
    refs.push(group ? group.info : { name, slug, url: '' });
  }
  return refs;
}

export function run(input) {
  const report = [];
  const templates = new Map();
  for (const name of Object.keys(input.templates).sort()) {
    const code = compile(input.templates[name]);
    if (typeof code === 'string') report.push(`template "${name}": ${code}`);
    else templates.set(name, code);
  }
  for (const code of templates.values()) {
    for (const instruction of code) {
      if (instruction.op === INCLUDE) instruction.value = templates.get(instruction.value);
    }
  }

  const base = input.site.base_url.replace(/\/+$/, '');
  const slugs = new Map();
  const usedByDir = new Map([['', new Map([['posts', 2], ['tags', 2], ['page', 2]])]]);
  const bySource = new Map();
  const staticPaths = new Set();
  const statics = [];
  const sources = [];
  const posts = [];
  const broken = new Set();

  const files = input.files.slice().sort((a, b) => compare(a.path, b.path));
  for (const file of files) {
    const { path, content } = file;
    if (!path.endsWith('.md')) {
      statics.push(file);
      staticPaths.add(path);
      continue;
    }

    const matter = readFrontMatter(content.includes('\r') ? content.replace(lineEndings, '\n') : content);
    if (typeof matter === 'string') {
      report.push(`refused ${path}: ${matter}`);
      continue;
    }
    const { data, body } = matter;
    const isPost = path.startsWith('posts/');
    const reserved = path.startsWith('tags/') ? 'tags' : path.startsWith('page/') ? 'page' : '';
    const layout = data.layout ?? (isPost ? 'post' : 'page');
    const error = reserved ? `${reserved}/ is the generator's own`
      : isPost && data.date === undefined ? 'a post needs a date'
      : !templates.has(layout) ? `layout "${layout}" is no template that parses` : '';
    if (error) {
      report.push(`refused ${path}: ${error}`);
      continue;
    }
    if (data.draft === true) {
      report.push(`draft ${path}`);
      continue;
    }

    const slash = path.lastIndexOf('/');
    const dir = slash < 0 ? '' : path.slice(0, slash);
    const outputDir = isPost ? 'posts' : dir;
    let used = usedByDir.get(outputDir);
    if (!used) usedByDir.set(outputDir, used = new Map());
    const slug = claimSlug(data.slug ?? slugOf(data.title, slugs), used);
    const permalink = outputDir ? outputDir + '/' + slug : slug;
    const url = `${base}/${permalink}/`;
    const source = { path, dir, data, body, layout, isPost, slug, permalink, url, link: null, card: null, page: null, tags: null, newer: null, older: null };
    sources.push(source);
    bySource.set(path, source);
    if (isPost) posts.push(source);
  }

  posts.sort((a, b) => compare(b.data.date, a.data.date) || compare(a.slug, b.slug));
  const byTag = new Map();
  for (let i = 0; i < posts.length; i++) {
    const post = posts[i];
    post.newer = posts[i - 1] ?? null;
    post.older = posts[i + 1] ?? null;
    post.tags = tagReferences(post.data.tags, slugs, byTag, post, base);
  }
  for (const source of sources) {
    const { data, body, path, dir, isPost, slug, url } = source;
    const rendered = markdown(body, slugs, sourceLink(path, dir, base, bySource, staticPaths, broken));
    const date = data.date ?? '';
    const dateLong = date ? `${+date.slice(8)} ${monthNames[+date.slice(5, 7) - 1]} ${date.slice(0, 4)}` : '';
    source.link = { title: data.title, url, date, date_long: dateLong };
    source.card = {
      ...source.link,
      summary: data.summary ?? '',
      excerpt: rendered.excerpt,
      reading_minutes: readingMinutes(body),
      tags: isPost ? source.tags : tagReferences(data.tags, slugs, byTag, null, base),
    };
    source.page = {
      ...source.card,
      slug,
      content: rendered.content,
      toc: rendered.toc,
      newer: null,
      older: null,
      is_post: isPost,
    };
  }

  const groups = [...byTag.values()].sort((a, b) =>
    b.posts.length - a.posts.length || compare(a.info.slug, b.info.slug));
  const cards = posts.map((post) => post.card);
  const common = {
    site: { title: input.site.title, base_url: base },
    tags: groups.map((group) => ({ ...group.info, count: group.posts.length })),
    recent: cards.slice(0, input.site.recent),
  };
  const output = {};
  const buffer = [];
  function render(code, root) {
    emit(code, 0, code.length, root, null, 0, buffer);
    const result = buffer.join('');
    buffer.length = 0;
    return result;
  }

  const pageRoot = { ...common, page: null };
  for (const source of sources) {
    source.page.newer = source.newer?.link ?? null;
    source.page.older = source.older?.link ?? null;
    pageRoot.page = source.page;
    output[source.permalink + '/index.html'] = render(templates.get(source.layout), pageRoot);
  }

  const index = templates.get('index');
  if (index) {
    const per = input.site.per_page;
    const total = Math.max(1, Math.ceil(posts.length / per));
    const indexUrl = (number) => number === 1 ? base + '/' : `${base}/page/${number}/`;
    const root = { ...common, posts: null, pager: null };
    for (let number = 1; number <= total; number++) {
      root.posts = cards.slice((number - 1) * per, number * per);
      root.pager = {
        number, total, url: indexUrl(number),
        newer_url: number === 1 ? '' : indexUrl(number - 1),
        older_url: number === total ? '' : indexUrl(number + 1),
      };
      output[number === 1 ? 'index.html' : `page/${number}/index.html`] = render(index, root);
    }
  }

  const tag = templates.get('tag');
  if (tag) {
    const root = { ...common, tag: null };
    for (let i = 0; i < groups.length; i++) {
      const group = groups[i];
      root.tag = { ...common.tags[i], posts: group.posts.map((post) => post.card) };
      output[`tags/${group.info.slug}/index.html`] = render(tag, root);
    }
  }
  const tags = templates.get('tags');
  if (tags) output['tags/index.html'] = render(tags, common);

  for (const { path, content } of statics) {
    if (Object.hasOwn(output, path)) report.push(`shadowed ${path}: a built page has its path`);
    else output[path] = content;
  }
  for (const line of broken) report.push(line);
  report.sort();
  report.push(`built ${sources.length - posts.length} pages, ${posts.length} posts, ${groups.length} tags and ${statics.length} static files\n`);
  output['build-report.txt'] = report.join('\n');
  return output;
}
