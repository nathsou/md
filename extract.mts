#!/usr/bin/env bun
// Extracts the exefs and romfs of the Program NCA inside a Nintendo Switch XCI (game card dump) or NSP (eShop
// package: its tickets supply the title keys). With --update <update.nsp>, the update's exefs and its romfs patched over
// the base game's (the update's BKTR section: an indirect storage mapping its romfs onto the base romfs and its own
// data, that data AES-CTR-Ex encrypted by subsection).
//
// Usage: bun extract.mts <game.xci|game.nsp> [outDir] [--update update.nsp] [--keys prod.keys] [--title-keys title.keys]
//
// outDir defaults to the dump's folder; keys default to prod.keys / title.keys next to the dump, then in the current
// folder. Writes <outDir>/exefs and <outDir>/romfs.

import { closeSync, existsSync, mkdirSync, openSync, readFileSync, readSync, statSync, writeSync } from "node:fs";
import { createCipheriv, createDecipheriv } from "node:crypto";
import { dirname, join, resolve } from "node:path";
import { parseArgs } from "node:util";

type Reader = (offset: number, length: number) => Buffer;
type Entry = { name: string; offset: number; size: number };

const { values: opts, positionals } = parseArgs({
  allowPositionals: true,
  options: { keys: { type: "string" }, "title-keys": { type: "string" }, update: { type: "string" } },
});
if (positionals.length < 1) {
  console.error("Usage: bun extract.mts <game.xci|game.nsp> [outDir] [--update update.nsp] [--keys prod.keys] [--title-keys title.keys]");
  process.exit(1);
}
const xciPath = resolve(positionals[0]);
const outDir = resolve(positionals[1] ?? dirname(xciPath));
const keyFile = (name: string) => [join(dirname(xciPath), name), resolve(name)].find((p) => existsSync(p)) ?? name;
const keys = loadKeys(opts.keys ?? keyFile("prod.keys"));
const titleKeys = loadKeys(opts["title-keys"] ?? keyFile("title.keys"));

function loadKeys(path: string): Map<string, Buffer> {
  const map = new Map<string, Buffer>();
  if (!existsSync(path)) return map;
  for (const line of readFileSync(path, "utf8").split(/\r?\n/)) {
    const m = line.match(/^\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*$/);
    if (m) map.set(m[1].toLowerCase(), Buffer.from(m[2], "hex"));
  }
  return map;
}

function needKey(name: string): Buffer {
  const key = keys.get(name);
  if (!key) throw new Error(`Missing key "${name}" in prod.keys`);
  return key;
}

// --- Crypto ------------------------------------------------------------------

function aesEcb(key: Buffer, data: Buffer, decrypt: boolean): Buffer {
  const c = (decrypt ? createDecipheriv : createCipheriv)("aes-128-ecb", key, null).setAutoPadding(false);
  return Buffer.concat([c.update(data), c.final()]);
}

// AES-128-XTS with Nintendo's big-endian sector tweak, 0x200-byte sectors starting at sector 0.
function decryptXts(enc: Buffer, key: Buffer): Buffer {
  const dataKey = key.subarray(0, 16);
  const tweakKey = key.subarray(16, 32);
  const out = Buffer.alloc(enc.length);
  for (let base = 0, sector = 0; base < enc.length; base += 0x200, sector++) {
    const tweakIn = Buffer.alloc(16);
    tweakIn.writeBigUInt64BE(BigInt(sector), 8);
    const tweak = aesEcb(tweakKey, tweakIn, false);
    const tweaks = Buffer.alloc(0x200);
    const xored = Buffer.alloc(0x200);
    for (let b = 0; b < 0x200; b += 16) {
      tweak.copy(tweaks, b);
      for (let i = 0; i < 16; i++) xored[b + i] = enc[base + b + i] ^ tweak[i];
      // Multiply the tweak by x in GF(2^128).
      let carry = 0;
      for (let i = 0; i < 16; i++) {
        const next = tweak[i] >> 7;
        tweak[i] = ((tweak[i] << 1) | carry) & 0xff;
        carry = next;
      }
      if (carry) tweak[0] ^= 0x87;
    }
    const dec = aesEcb(dataKey, xored, true);
    for (let i = 0; i < 0x200; i++) out[base + i] = dec[i] ^ tweaks[i];
  }
  return out;
}

// --- Readers -----------------------------------------------------------------

const openFds: number[] = [];
function fileReader(path: string): Reader {
  const fd = openSync(path, "r");
  openFds.push(fd);
  return (offset, length) => {
    const buf = Buffer.allocUnsafe(length);
    for (let done = 0; done < length; ) {
      const n = readSync(fd, buf, done, length - done, offset + done);
      if (n === 0) throw new Error(`Unexpected end of file at 0x${(offset + done).toString(16)}`);
      done += n;
    }
    return buf;
  };
}
const readRaw: Reader = fileReader(xciPath);

const sub = (read: Reader, base: number): Reader => (offset, length) => read(base + offset, length);

// AES-128-CTR whose counter is the absolute NCA offset / 16, upper half from the FS header (AES-CTR-Ex: its bytes 4-7
// replaced by the subsection's generation).
function ctrReader(read: Reader, key: Buffer, upperIv: Buffer, generation?: number): Reader {
  return (offset, length) => {
    const aligned = offset - (offset % 16);
    const iv = Buffer.alloc(16);
    for (let i = 0; i < 8; i++) iv[i] = upperIv[7 - i];
    if (generation !== undefined) iv.writeUInt32BE(generation >>> 0, 4);
    iv.writeBigUInt64BE(BigInt(aligned / 16), 8);
    const d = createDecipheriv("aes-128-ctr", key, iv);
    const dec = d.update(read(aligned, offset - aligned + length));
    return dec.subarray(offset - aligned);
  };
}

// --- Formats -----------------------------------------------------------------

function safeName(name: string): string {
  if (!name || name === "." || name === ".." || /[\/\\\0]/.test(name)) throw new Error(`Unsafe file name: ${JSON.stringify(name)}`);
  return name;
}

// HFS0 (gamecard partitions) and PFS0 (exefs) share a layout apart from the entry size.
function parsePartitionFs(read: Reader, magic: "HFS0" | "PFS0"): Entry[] {
  const head = read(0, 0x10);
  const found = head.toString("latin1", 0, 4);
  if (found !== magic) throw new Error(`Expected ${magic}, found ${JSON.stringify(found)}`);
  const count = head.readUInt32LE(4);
  const entrySize = magic === "HFS0" ? 0x40 : 0x18;
  const table = read(0x10, count * entrySize + head.readUInt32LE(8));
  const strings = table.subarray(count * entrySize);
  const dataStart = 0x10 + table.length;
  return Array.from({ length: count }, (_, i) => {
    const e = table.subarray(i * entrySize);
    const nameOff = e.readUInt32LE(0x10);
    return {
      name: strings.toString("utf8", nameOff, strings.indexOf(0, nameOff)),
      offset: dataStart + Number(e.readBigUInt64LE(0)),
      size: Number(e.readBigUInt64LE(8)),
    };
  });
}

const CONTENT_TYPES = ["Program", "Meta", "Control", "Manual", "Data", "PublicData"];
const KEY_AREA_NAMES = ["application", "ocean", "system"];

// data: the filesystem (the last hash level); storage: the whole decrypted section from its start (a base romfs, for an
// update's patch); patch: an update's BKTR section, read through applyPatch
type Section = {
  index: number;
  fsType: number;
  data?: Reader;
  storage?: Reader;
  dataOffset?: number;
  patch?: { fs: Buffer; read: Reader; start: number; key: Buffer };
  error?: string;
};
type Nca = { name: string; size: number; contentType: string; sections: Section[] };

function openNca(read: Reader, name: string, size: number): Nca {
  const h = decryptXts(read(0, 0xc00), needKey("header_key"));
  const magic = h.toString("latin1", 0x200, 0x204);
  if (magic !== "NCA3") throw new Error(`${name}: unsupported NCA magic ${JSON.stringify(magic)} (bad header_key?)`);

  const keyGen = Math.max(h[0x206], h[0x220]);
  const rev = Math.max(keyGen - 1, 0).toString(16).padStart(2, "0");
  const rightsId = h.subarray(0x230, 0x240);
  let ctrKey: Buffer;
  if (rightsId.some((b) => b !== 0)) {
    const titleKey = titleKeys.get(rightsId.toString("hex"));
    if (!titleKey) throw new Error(`${name}: missing title key for rights ID ${rightsId.toString("hex")}`);
    ctrKey = aesEcb(needKey(`titlekek_${rev}`), titleKey, true);
  } else {
    const kak = needKey(`key_area_key_${KEY_AREA_NAMES[h[0x207]]}_${rev}`);
    ctrKey = aesEcb(kak, h.subarray(0x320, 0x330), true);
  }

  const sections: Section[] = [];
  for (let i = 0; i < 4; i++) {
    const start = h.readUInt32LE(0x240 + i * 0x10) * 0x200;
    const end = h.readUInt32LE(0x244 + i * 0x10) * 0x200;
    if (end === 0) continue;
    const fs = h.subarray(0x400 + i * 0x200, 0x600 + i * 0x200);
    const [fsType, hashType, encType] = [fs[2], fs[3], fs[4]];
    const hashData = fs.subarray(0x8, 0x100);

    let dataOffset: number;
    if (hashType === 2) {
      // HierarchicalSha256: the last layer region is the filesystem.
      const layer = hashData.readUInt32LE(0x24) - 1;
      dataOffset = Number(hashData.readBigUInt64LE(0x28 + layer * 0x10));
    } else if (hashType === 3) {
      // HierarchicalIntegrity (IVFC): the last level is the filesystem.
      const level = hashData.readUInt32LE(0xc) - 2;
      dataOffset = Number(hashData.readBigUInt64LE(0x10 + level * 0x18));
    } else {
      sections.push({ index: i, fsType, error: `unsupported hash type ${hashType}` });
      continue;
    }

    let decrypted: Reader;
    if (encType === 1) decrypted = read;
    else if (encType === 3) decrypted = ctrReader(read, ctrKey, fs.subarray(0x140, 0x148));
    else if (encType === 4) {
      // AES-CTR-Ex (an update's BKTR romfs): only readable over the base game's romfs (applyPatch)
      sections.push({ index: i, fsType, dataOffset, patch: { fs: Buffer.from(fs), read, start, key: ctrKey } });
      continue;
    } else {
      sections.push({ index: i, fsType, error: `unsupported encryption type ${encType}` });
      continue;
    }
    sections.push({ index: i, fsType, data: sub(decrypted, start + dataOffset), storage: sub(decrypted, start), dataOffset });
  }
  return { name, size, contentType: CONTENT_TYPES[h[0x205]] ?? `Unknown(${h[0x205]})`, sections };
}

// --- Updates (BKTR) ----------------------------------------------------------
// The patch section's tables (at the offsets its FS header's patch info gives, inside the section, plain AES-CTR):
// bucket trees of 0x4000-byte nodes - a header {u32, u32 bucket count, u64 total size}, the buckets' start offsets,
// then the buckets {u32, u32 entry count, u64 end offset, entries}. Indirect entries (0x14): {u64 virtual offset, u64
// physical offset, u32 storage: 0 the base romfs, 1 this section}; AES-CTR-Ex entries (0x10): {u64 offset, u32 size,
// u32 generation}.
function readBucketTree(read: Reader, size: number, entrySize: number): Buffer[] {
  const head = read(0, 0x4000);
  const buckets = head.readUInt32LE(4);
  const out: Buffer[] = [];
  for (let b = 0; b < buckets; b++) {
    const bucket = read(0x4000 * (b + 1), 0x4000);
    const n = bucket.readUInt32LE(4);
    for (let e = 0; e < n; e++) out.push(Buffer.from(bucket.subarray(0x10 + e * entrySize, 0x10 + (e + 1) * entrySize)));
  }
  if (0x4000 * (buckets + 1) > size) throw new Error("BKTR: bucket tree larger than its table");
  return out;
}

function applyPatch(p: NonNullable<Section["patch"]>, dataOffset: number, base: Reader): Reader {
  const upper = p.fs.subarray(0x140, 0x148);
  const plain = ctrReader(p.read, p.key, upper);  // (the tables)
  const info = p.fs.subarray(0x100, 0x140);
  const indOff = Number(info.readBigUInt64LE(0)), indSize = Number(info.readBigUInt64LE(8));
  const ctrOff = Number(info.readBigUInt64LE(0x20)), ctrSize = Number(info.readBigUInt64LE(0x28));
  if (info.toString("latin1", 0x10, 0x14) !== "BKTR" || info.toString("latin1", 0x30, 0x34) !== "BKTR")
    throw new Error("update romfs: no BKTR patch info");
  const ind = readBucketTree(sub(plain, p.start + indOff), indSize, 0x14).map((e) => ({
    virt: Number(e.readBigUInt64LE(0)),
    phys: Number(e.readBigUInt64LE(8)),
    storage: e.readUInt32LE(0x10),
  }));
  const subs = readBucketTree(sub(plain, p.start + ctrOff), ctrSize, 0x10).map((e) => ({
    off: Number(e.readBigUInt64LE(0)),
    gen: e.readUInt32LE(0xc),
  }));
  subs.push({ off: indOff, gen: 0 });  // (the tables themselves: plain CTR)
  const find = <T extends { [k: string]: number }>(arr: T[], key: keyof T, v: number) => {
    let lo = 0, hi = arr.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (arr[m][key] <= v) lo = m; else hi = m - 1; }
    return lo;
  };
  // patch data at section offset o (length n), decrypted subsection by subsection
  const readPatch = (o: number, n: number): Buffer => {
    const parts: Buffer[] = [];
    while (n > 0) {
      const s = find(subs, "off", o);
      const end = s + 1 < subs.length ? subs[s + 1].off : Infinity;
      const take = Math.min(n, end - o);
      parts.push(ctrReader(p.read, p.key, upper, subs[s].gen)(p.start + o, take));
      o += take, n -= take;
    }
    return parts.length === 1 ? parts[0] : Buffer.concat(parts);
  };
  const virtual: Reader = (o, n) => {
    const parts: Buffer[] = [];
    while (n > 0) {
      const i = find(ind, "virt", o);
      const end = i + 1 < ind.length ? ind[i + 1].virt : Infinity;
      const take = Math.min(n, end - o);
      const at = ind[i].phys + (o - ind[i].virt);
      parts.push(ind[i].storage === 0 ? base(at, take) : readPatch(at, take));
      o += take, n -= take;
    }
    return parts.length === 1 ? parts[0] : Buffer.concat(parts);
  };
  return sub(virtual, dataOffset);
}

// --- Extraction --------------------------------------------------------------

const CHUNK = 16 << 20;
let filesWritten = 0;
let bytesWritten = 0;
let lastReport = 0;

function copyOut(read: Reader, offset: number, size: number, dest: string) {
  const out = openSync(dest, "w");
  try {
    for (let done = 0; done < size; ) {
      const buf = read(offset + done, Math.min(CHUNK, size - done));
      for (let w = 0; w < buf.length; ) w += writeSync(out, buf, w);
      done += buf.length;
    }
  } finally {
    closeSync(out);
  }
  filesWritten++;
  bytesWritten += size;
  if (bytesWritten - lastReport >= 256 << 20) {
    lastReport = bytesWritten;
    process.stdout.write(`\r  ${filesWritten} files, ${(bytesWritten / 2 ** 30).toFixed(2)} GiB`);
  }
}

function extractPfs0(read: Reader, dir: string) {
  mkdirSync(dir, { recursive: true });
  for (const e of parsePartitionFs(read, "PFS0")) copyOut(read, e.offset, e.size, join(dir, safeName(e.name)));
}

function extractRomfs(read: Reader, dir: string) {
  const h = read(0, 0x50);
  const u64 = (o: number) => Number(h.readBigUInt64LE(o));
  const dirMeta = read(u64(0x18), u64(0x20));
  const fileMeta = read(u64(0x38), u64(0x40));
  const dataOffset = u64(0x48);
  const NONE = 0xffffffff;

  const walk = (dirEntry: number, path: string) => {
    mkdirSync(path, { recursive: true });
    for (let f = dirMeta.readUInt32LE(dirEntry + 0xc); f !== NONE; f = fileMeta.readUInt32LE(f + 0x4)) {
      const name = fileMeta.toString("utf8", f + 0x20, f + 0x20 + fileMeta.readUInt32LE(f + 0x1c));
      const offset = dataOffset + Number(fileMeta.readBigUInt64LE(f + 0x8));
      copyOut(read, offset, Number(fileMeta.readBigUInt64LE(f + 0x10)), join(path, safeName(name)));
    }
    for (let d = dirMeta.readUInt32LE(dirEntry + 0x8); d !== NONE; d = dirMeta.readUInt32LE(d + 0x4)) {
      const name = dirMeta.toString("utf8", d + 0x18, d + 0x18 + dirMeta.readUInt32LE(d + 0x14));
      walk(d, join(path, safeName(name)));
    }
  };
  walk(0, dir);
}

// --- Main --------------------------------------------------------------------

// A ticket (.tik): its signature block's size depends on the signature type; then the issuer (0x40), the title key
// block (0x100: a common ticket's title key in the first 16 bytes) and, at +0x160 from the issuer, the rights ID.
function readTicket(t: Buffer) {
  const sigType = t.readUInt32LE(0);
  const sigSize = ({ 0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3c, 0x10003: 0x200, 0x10004: 0x100, 0x10005: 0x3c } as Record<number, number>)[sigType];
  if (sigSize === undefined) throw new Error(`ticket: unknown signature type 0x${sigType.toString(16)}`);
  const body = 4 + sigSize + ((0x40 - ((4 + sigSize) % 0x40)) % 0x40);
  if (t[body + 0x141] !== 0) throw new Error("ticket: personalized (console-bound) title key: not supported");
  titleKeys.set(t.toString("hex", body + 0x160, body + 0x170), Buffer.from(t.subarray(body + 0x40, body + 0x50)));
}

function openNsp(read: Reader, path: string): Nca[] {  // a PFS0 of NCAs, tickets and certificates
  const entries = parsePartitionFs(read, "PFS0");
  const need = Math.max(...entries.map((e) => e.offset + e.size)), have = statSync(path).size;
  if (have < need)
    throw new Error(`${path} is truncated: ${have} bytes of the ${need} its file table lists (an incomplete download?)`);
  for (const e of entries.filter((e) => e.name.endsWith(".tik"))) readTicket(read(e.offset, e.size));
  return entries.filter((e) => e.name.endsWith(".nca")).map((e) => openNca(sub(read, e.offset), e.name, e.size));
}

let ncas: Nca[];
const magic = readRaw(0, 4).toString("latin1");
if (magic === "PFS0") {
  ncas = openNsp(readRaw, xciPath);
  console.log("NSP NCAs:");
} else {
  // XCI. Some dumps prepend a 0x1000-byte key area before the card header.
  const base = readRaw(0x100, 4).toString("latin1") === "HEAD" ? 0 : 0x1000;
  if (readRaw(base + 0x100, 4).toString("latin1") !== "HEAD") throw new Error("Not an XCI (no HEAD magic) or NSP (no PFS0 magic)");
  const rootOffset = base + Number(readRaw(base + 0x130, 8).readBigUInt64LE(0));
  const secure = parsePartitionFs(sub(readRaw, rootOffset), "HFS0").find((e) => e.name === "secure");
  if (!secure) throw new Error("XCI has no secure partition");
  const secureRead = sub(readRaw, rootOffset + secure.offset);
  const entries = parsePartitionFs(secureRead, "HFS0").filter((e) => e.name.endsWith(".nca"));
  // (the card header's last valid page, at 0x118: a dump shorter than that is cut short)
  const need = base + (readRaw(base + 0x118, 4).readUInt32LE(0) + 1) * 0x200, have = statSync(xciPath).size;
  if (have < need) {
    const at = (e: Entry) => rootOffset + secure.offset + e.offset;
    const lines = entries.map((e) => {
      const got = Math.max(0, Math.min(e.size, have - at(e)));
      return `  ${e.name}  ${(e.size / 2 ** 20).toFixed(1)} MiB: ${got === e.size ? "complete" : `${(got / 2 ** 20).toFixed(1)} MiB present`}`;
    });
    throw new Error(`${xciPath} is truncated: ${have} bytes of the ${need} its card header lists (an incomplete ` +
                    `download or copy?)\n${lines.join("\n")}`);
  }
  ncas = entries.map((e) => openNca(sub(secureRead, e.offset), e.name, e.size));
  console.log("Secure partition NCAs:");
}
for (const n of ncas) console.log(`  ${n.name}  ${n.contentType.padEnd(10)} ${(n.size / 2 ** 20).toFixed(1)} MiB`);

const programs = ncas.filter((n) => n.contentType === "Program").sort((a, b) => b.size - a.size);
if (programs.length === 0) throw new Error("No Program NCA found");
if (programs.length > 1) console.warn(`Multiple Program NCAs; using the largest (${programs[0].name})`);
const program = programs[0];

for (const s of program.sections) if (s.error) console.warn(`  section ${s.index}: ${s.error}`);
const findExefs = (p: Nca) => {
  const pfs = p.sections.filter((s) => s.fsType === 1 && s.data);
  return pfs.find((s) => parsePartitionFs(s.data!, "PFS0").some((e) => e.name === "main.npdm")) ?? pfs[0];
};
let exefs = findExefs(program);
let romfs = program.sections.find((s) => s.fsType === 0 && s.data);

if (opts.update) {
  const updNcas = openNsp(fileReader(resolve(opts.update)), resolve(opts.update));
  console.log("Update NCAs:");
  for (const n of updNcas) console.log(`  ${n.name}  ${n.contentType.padEnd(10)} ${(n.size / 2 ** 20).toFixed(1)} MiB`);
  const up = updNcas.filter((n) => n.contentType === "Program").sort((a, b) => b.size - a.size)[0];
  if (!up) throw new Error("The update has no Program NCA");
  exefs = findExefs(up) ?? exefs;
  const patch = up.sections.find((s) => s.fsType === 0 && s.patch);
  if (patch && romfs?.storage) {
    console.log(`Patching the romfs with the update (section ${patch.index})`);
    romfs = { index: patch.index, fsType: 0, data: applyPatch(patch.patch!, patch.dataOffset!, romfs.storage) };
  } else if (patch) throw new Error("The update patches a romfs the base game doesn't have");
  else console.warn("The update has no romfs patch: the base romfs as is");
}

if (exefs) {
  console.log(`Extracting exefs (section ${exefs.index}) to ${join(outDir, "exefs")}`);
  extractPfs0(exefs.data!, join(outDir, "exefs"));
} else console.warn("No exefs section found");

if (romfs) {
  console.log(`Extracting romfs (section ${romfs.index}) to ${join(outDir, "romfs")}`);
  extractRomfs(romfs.data!, join(outDir, "romfs"));
} else console.warn("No romfs section found");

for (const f of openFds) closeSync(f);
console.log(`\rDone: ${filesWritten} files, ${(bytesWritten / 2 ** 30).toFixed(2)} GiB written.`);
