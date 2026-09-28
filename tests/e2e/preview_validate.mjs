import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const P = require('../../web/js/core/preview.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (arguments[2] !== undefined ? '  <' + arguments[2] + '>' : '')); }
}

console.log('preview.js 单元测试 (纯逻辑)');

// ---- previewCat / isImage ----
check(P.previewCat('a.png') === 'image', 'png -> image');
check(P.previewCat('a.MP4') === 'video', 'MP4 -> video (大小写)');
check(P.previewCat('a.mp3') === 'audio', 'mp3 -> audio');
check(P.previewCat('a.pdf') === 'pdf', 'pdf -> pdf');
check(P.previewCat('a.md') === 'markdown', 'md -> markdown');
check(P.previewCat('a.cpp') === 'text', 'cpp -> text');
check(P.previewCat('a.bin') === null, '未知 -> null');
check(P.isImage('x.jpeg') === true, 'isImage jpeg');
check(P.isImage('x.mp4') === false, 'isImage 否定');

// ---- safeUrl ----
check(P.safeUrl('https://a.com') === 'https://a.com', 'safeUrl 允许 https');
check(P.safeUrl('/rel/path') === '/rel/path', 'safeUrl 允许相对路径');
check(P.safeUrl('javascript:alert(1)') === '#', 'safeUrl 拦截 javascript:');
check(P.safeUrl('data:text/html,x') === '#', 'safeUrl 拦截 data:');

// ---- renderMarkdown (XSS 转义 + 语法) ----
const h1 = P.renderMarkdown('# Title');
check(h1 === '<h1>Title</h1>', '标题渲染', h1);
const bold = P.renderMarkdown('a **b** c');
check(bold.includes('<strong>b</strong>'), '加粗渲染', bold);
const li = P.renderMarkdown('- one\n- two');
check(li.includes('<ul>') && li.includes('<li>one</li>') && li.includes('<li>two</li>'), '无序列表渲染', li);
const code = P.renderMarkdown('```\n<x>\n```');
check(code.includes('<pre class="md-code">') && code.includes('&lt;x&gt;'), '代码块转义', code);
const xss = P.renderMarkdown('<script>alert(1)</' + 'script>');
check(!xss.includes('<script>') && xss.includes('&lt;script&gt;'), 'XSS 标签被转义', xss);
const link = P.renderMarkdown('[t](javascript:alert(1))');
check(!link.includes('javascript:') && link.includes('href="#"'), 'Markdown 链接拦截危险协议', link);

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
