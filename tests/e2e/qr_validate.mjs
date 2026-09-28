import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const jsQR = require('jsqr');
const QR = require('../../web/js/qr.js');

const scale = 6, margin = 4;
function toRGBA(q) {
  const dim = (q.size + margin*2) * scale;
  const data = new Uint8ClampedArray(dim*dim*4);
  data.fill(255); // white
  for (let r=0;r<q.size;r++) for (let c=0;c<q.size;c++) if (q.modules[r][c]) {
    for (let dy=0;dy<scale;dy++) for (let dx=0;dx<scale;dx++) {
      const y=(r+margin)*scale+dy, x=(c+margin)*scale+dx, i=(y*dim+x)*4;
      data[i]=0;data[i+1]=0;data[i+2]=0;data[i+3]=255;
    }
  }
  return { data, dim };
}
const cases = [
  'https://example.com',
  'http://192.168.103.11:8888/share.html?t=nmyBNTfnBBKKS7wC82eUeK',
  'CloudVault 云库 分享: 提取码 ab12',
  'a'.repeat(50),
  'x'.repeat(100), 'y'.repeat(115), 'z'.repeat(150), 'w'.repeat(170), 'q'.repeat(205), '中文测试'.repeat(10),
  'http://192.168.103.11:8888/share.html?t=' + 'Z9kQ2mNpQ7'.repeat(3),
];
let pass=0, fail=0;
for (const t of cases) {
  try {
    const q = QR.generate(t);
    const { data, dim } = toRGBA(q);
    const res = jsQR(data, dim, dim);
    if (res && res.data === t) { pass++; console.log('  \u2713 v'+q.version+' mask'+q.mask+' len='+t.length); }
    else { fail++; console.log('  \u2717 decode mismatch len='+t.length+' got='+(res?JSON.stringify(res.data.slice(0,30)):'null')); }
  } catch(e){ fail++; console.log('  \u2717 error len='+t.length+' '+e.message); }
}
console.log('\nQR: '+pass+' 通过, '+fail+' 失败');
process.exit(fail?1:0);
