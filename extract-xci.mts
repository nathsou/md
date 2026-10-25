#!/usr/bin/env bun
// Extracts the exefs and romfs of the Program NCA inside a Nintendo Switch XCI.
//
// Usage: bun extract-xci.mts <game.xci> [outDir] [--keys prod.keys] [--title-keys title.keys]
//
// outDir defaults to the XCI's folder; keys default to prod.keys / title.keys next to the XCI.
// Writes <outDir>/exefs and <outDir>/romfs.

import { closeSync, existsSync, mkdirSync, openSync, readFileSync, readSync, writeSync } from "node:fs";
import { createCipheriv, createDecipheriv } from "node:crypto";
import { dirname, join, resolve } from "node:path";
import { parseArgs } from "node:util";

type Reader = (offset: number, length: number) => Buffer;
type Entry = { name: string; offset: number; size: number };

const { values: opts, positionals } = parseArgs({
  allowPositionals: true,
  options: { keys: { type: "string" }, "title-keys": { type: "string" } },
});
if (positionals.length < 1) {
  console.error("Usage: bun extract-xci.mts <game.xci> [outDir] [--keys prod.keys] [--title-keys title.keys]");
  process.exit(1);
}
const xciPath = resolve(positionals[0]);
const outDir = resolve(positionals[1] ?? dirname(xciPath));
const keys = loadKeys(opts.keys ?? join(dirname(xciPath), "prod.keys"));
const titleKeys = loadKeys(opts["title-keys"] ?? join(dirname(xciPath), "title.keys"));

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

const fd = openSync(xciPath, "r");

const readRaw: Reader = (offset, length) => {
  const buf = Buffer.allocUnsafe(length);
  for (let done = 0; done < length; ) {
    const n = readSync(fd, buf, done, length - done, offset + done);
    if (n === 0) throw new Error(`Unexpected end of file at 0x${(offset + done).toString(16)}`);
    done += n;
  }
  return buf;
};

const sub = (read: Reader, base: number): Reader => (offset, length) => read(base + offset, length);

// AES-128-CTR whose counter is the absolute NCA offset / 16, upper half from the FS header.
function ctrReader(read: Reader, key: Buffer, upperIv: Buffer): Reader {
  return (offset, length) => {
    const aligned = offset - (offset % 16);
    const iv = Buffer.alloc(16);
    for (let i = 0; i < 8; i++) iv[i] = upperIv[7 - i];
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

type Section = { index: number; fsType: number; data?: Reader; error?: string };
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
    else {
      sections.push({ index: i, fsType, error: `unsupported encryption type ${encType}` });
      continue;
    }
    sections.push({ index: i, fsType, data: sub(decrypted, start + dataOffset) });
  }
  return { name, size, contentType: CONTENT_TYPES[h[0x205]] ?? `Unknown(${h[0x205]})`, sections };
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

// Some dumps prepend a 0x1000-byte key area before the card header.
const base = readRaw(0x100, 4).toString("latin1") === "HEAD" ? 0 : 0x1000;
if (readRaw(base + 0x100, 4).toString("latin1") !== "HEAD") throw new Error("Not an XCI file (no HEAD magic)");
const rootOffset = base + Number(readRaw(base + 0x130, 8).readBigUInt64LE(0));
const secure = parsePartitionFs(sub(readRaw, rootOffset), "HFS0").find((e) => e.name === "secure");
if (!secure) throw new Error("XCI has no secure partition");
const secureRead = sub(readRaw, rootOffset + secure.offset);

const ncas = parsePartitionFs(secureRead, "HFS0")
  .filter((e) => e.name.endsWith(".nca"))
  .map((e) => openNca(sub(secureRead, e.offset), e.name, e.size));
console.log("Secure partition NCAs:");
for (const n of ncas) console.log(`  ${n.name}  ${n.contentType.padEnd(10)} ${(n.size / 2 ** 20).toFixed(1)} MiB`);

const programs = ncas.filter((n) => n.contentType === "Program").sort((a, b) => b.size - a.size);
if (programs.length === 0) throw new Error("No Program NCA found");
if (programs.length > 1) console.warn(`Multiple Program NCAs; using the largest (${programs[0].name})`);
const program = programs[0];

for (const s of program.sections) if (s.error) console.warn(`  section ${s.index}: ${s.error}`);
const pfsSections = program.sections.filter((s) => s.fsType === 1 && s.data);
const exefs =
  pfsSections.find((s) => parsePartitionFs(s.data!, "PFS0").some((e) => e.name === "main.npdm")) ?? pfsSections[0];
const romfs = program.sections.find((s) => s.fsType === 0 && s.data);

if (exefs) {
  console.log(`Extracting exefs (section ${exefs.index}) to ${join(outDir, "exefs")}`);
  extractPfs0(exefs.data!, join(outDir, "exefs"));
} else console.warn("No exefs section found");

if (romfs) {
  console.log(`Extracting romfs (section ${romfs.index}) to ${join(outDir, "romfs")}`);
  extractRomfs(romfs.data!, join(outDir, "romfs"));
} else console.warn("No romfs section found");

closeSync(fd);
console.log(`\rDone: ${filesWritten} files, ${(bytesWritten / 2 ** 30).toFixed(2)} GiB written.`);
