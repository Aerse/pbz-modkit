// glb2bin: decode a (meshopt-compressed) glb into a flat file the sniper mod reads at runtime.
//   node glb2bin.mjs <meshopt_decoder.mjs> <in.glb> <out.bin>
// out: "AWPM" u32 nv u32 ni, then nv*3 f32 pos, nv*3 f32 normal, nv*2 f32 uv, ni u32 indices.
// Positions stay in the glb's own units/axes; the mod converts them.
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';
const [decPath, inPath, outPath] = process.argv.slice(2);
const { MeshoptDecoder } = await import(pathToFileURL(decPath).href);
await MeshoptDecoder.ready;
const b = fs.readFileSync(inPath);
const jl = b.readUInt32LE(12), j = JSON.parse(b.subarray(20, 20 + jl).toString());
const bin = b.subarray(20 + jl + 8);
const views = j.bufferViews.map(v => {
  const e = v.extensions?.EXT_meshopt_compression;
  if (!e) return bin.subarray(v.byteOffset || 0, (v.byteOffset || 0) + v.byteLength);
  const out = new Uint8Array(e.count * e.byteStride);
  const src = bin.subarray(e.byteOffset || 0, (e.byteOffset || 0) + e.byteLength);
  MeshoptDecoder.decodeGltfBuffer(out, e.count, e.byteStride, src, e.mode, e.filter);
  return out;
});
const acc = i => { const a = j.accessors[i], v = views[a.bufferView], o = (a.byteOffset || 0) + v.byteOffset;
  const n = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 }[a.type] * a.count;
  const T = { 5126: Float32Array, 5125: Uint32Array, 5123: Uint16Array }[a.componentType];
  return new T(v.buffer.slice(o, o + n * T.BYTES_PER_ELEMENT)); };
const p = j.meshes[0].primitives[0];
const pos = acc(p.attributes.POSITION), nor = acc(p.attributes.NORMAL), uv = acc(p.attributes.TEXCOORD_0);
const idx = Uint32Array.from(acc(p.indices));
const nv = pos.length / 3, ni = idx.length;
const h = Buffer.alloc(12); h.write('AWPM'); h.writeUInt32LE(nv, 4); h.writeUInt32LE(ni, 8);
fs.writeFileSync(outPath, Buffer.concat([h, Buffer.from(pos.buffer), Buffer.from(nor.buffer), Buffer.from(uv.buffer), Buffer.from(idx.buffer)]));
let mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
for (let i = 0; i < nv; i++) for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], pos[i*3+k]); mx[k] = Math.max(mx[k], pos[i*3+k]); }
console.log(`verts ${nv} tris ${ni/3} min ${mn.map(x=>x.toFixed(2))} max ${mx.map(x=>x.toFixed(2))}`);
