#!/usr/bin/env node

import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS, KHRDracoMeshCompression } from '@gltf-transform/extensions';
import { dedup, prune, simplify, weld } from '@gltf-transform/functions';
import draco3d from 'draco3dgltf';
import { MeshoptSimplifier } from 'meshoptimizer';

const MIB = 1024 * 1024;
// These passes preserve every primitive. If they cannot meet the limit, the
// model is split losslessly at primitive boundaries instead of deleting parts.
const SIMPLIFY_RATIOS = [0.80, 0.60, 0.42, 0.28, 0.16, 0.08, 0.03, 0.012];

function usage() {
  console.log(`Draco GLB size limit compressor

Usage:
  node compress-glb.mjs <input> <output> [options]

Options:
  --limit-mib N    Target size in MiB (default: 4)
  --large-only     Do not copy files already under the limit
  --overwrite      Replace existing output files
  --split-only     Preserve original geometry; split oversized GLBs directly
  -h, --help       Show help

For a complete 3D Tiles dataset, pass the directory containing tileset.json.
When a GLB must be split, references in output JSON files are updated to the
3D Tiles 1.1 "contents" array automatically.`);
}

function parseArgs(argv) {
  if (argv.includes('-h') || argv.includes('--help')) return null;
  if (argv.length < 2) throw new Error('Input and output paths are required.');
  const options = {
    input: path.resolve(argv[0]),
    output: path.resolve(argv[1]),
    limit: 4 * MIB,
    copySmall: true,
    overwrite: false,
    splitOnly: false,
  };
  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--limit-mib' && i + 1 < argv.length) {
      const value = Number(argv[++i]);
      if (!(value > 0)) throw new Error('--limit-mib must be greater than zero.');
      options.limit = Math.floor(value * MIB);
    } else if (arg === '--large-only') {
      options.copySmall = false;
    } else if (arg === '--overwrite') {
      options.overwrite = true;
    } else if (arg === '--split-only') {
      options.splitOnly = true;
    } else {
      throw new Error(`Unknown or incomplete option: ${arg}`);
    }
  }
  return options;
}

function sizeText(bytes) {
  return `${(bytes / MIB).toFixed(2)} MiB`;
}

function pathIsInside(child, parent) {
  const relative = path.relative(parent, child);
  return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative));
}

function normalizedKey(file) {
  const resolved = path.resolve(file);
  return process.platform === 'win32' ? resolved.toLowerCase() : resolved;
}

async function exists(file) {
  try {
    await fs.access(file);
    return true;
  } catch {
    return false;
  }
}

async function* walk(root) {
  const entries = await fs.readdir(root, { withFileTypes: true });
  for (const entry of entries) {
    const absolute = path.join(root, entry.name);
    if (entry.isDirectory()) yield* walk(absolute);
    else if (entry.isFile()) yield absolute;
  }
}

async function collectFiles(root) {
  const result = [];
  for await (const file of walk(root)) result.push(file);
  return result;
}

async function copyFile(source, destination, overwrite) {
  await fs.mkdir(path.dirname(destination), { recursive: true });
  if (!overwrite && await exists(destination)) return;
  await fs.copyFile(source, destination);
}

async function createIO() {
  await MeshoptSimplifier.ready;
  const [decoder, encoder] = await Promise.all([
    draco3d.createDecoderModule(),
    draco3d.createEncoderModule(),
  ]);
  return new NodeIO()
    .registerExtensions(ALL_EXTENSIONS)
    .registerDependencies({
      'draco3d.decoder': decoder,
      'draco3d.encoder': encoder,
    });
}

function configureDraco(document) {
  let draco = document.getRoot().listExtensionsUsed()
    .find((extension) => extension.extensionName === KHRDracoMeshCompression.EXTENSION_NAME);
  if (!draco) draco = document.createExtension(KHRDracoMeshCompression);
  draco.setRequired(true).setEncoderOptions({
    method: KHRDracoMeshCompression.EncoderMethod.EDGEBREAKER,
    encodeSpeed: 5,
    decodeSpeed: 5,
    quantizationBits: {
      POSITION: 11,
      NORMAL: 9,
      TEXCOORD: 11,
      COLOR: 8,
      GENERIC: 10,
    },
    quantizationVolume: 'mesh',
  });
}

function stripUnlitNormals(document) {
  for (const mesh of document.getRoot().listMeshes()) {
    for (const primitive of mesh.listPrimitives()) {
      const material = primitive.getMaterial();
      if (material?.getExtension('KHR_materials_unlit')) {
        // KHR_materials_unlit ignores normals and tangents, so removing them is
        // visually lossless and reduces Draco payload size.
        primitive.setAttribute('NORMAL', null);
        primitive.setAttribute('TANGENT', null);
      }
    }
  }
}

function listPrimitiveEntries(document) {
  const entries = [];
  let index = 0;
  for (const mesh of document.getRoot().listMeshes()) {
    for (const primitive of mesh.listPrimitives()) {
      const position = primitive.getAttribute('POSITION');
      const vertexCount = position?.getCount() ?? 0;
      const indexCount = primitive.getIndices()?.getCount() ?? vertexCount;
      let imageBytes = 0;
      const material = primitive.getMaterial();
      const textures = [
        material?.getBaseColorTexture(),
        material?.getMetallicRoughnessTexture(),
        material?.getNormalTexture(),
        material?.getOcclusionTexture(),
        material?.getEmissiveTexture(),
      ];
      for (const texture of textures) imageBytes += texture?.getImage()?.byteLength ?? 0;
      entries.push({ index: index++, weight: indexCount * 4 + vertexCount * 16 + imageBytes });
    }
  }
  return entries;
}

function retainPrimitiveIndices(document, selected) {
  let index = 0;
  for (const mesh of document.getRoot().listMeshes()) {
    for (const primitive of [...mesh.listPrimitives()]) {
      if (!selected.has(index)) mesh.removePrimitive(primitive);
      index++;
    }
  }
}

function splitBalanced(entries) {
  const sorted = [...entries].sort((a, b) => b.weight - a.weight);
  const left = [];
  const right = [];
  let leftWeight = 0;
  let rightWeight = 0;
  for (const entry of sorted) {
    if (leftWeight <= rightWeight) {
      left.push(entry);
      leftWeight += entry.weight;
    } else {
      right.push(entry);
      rightWeight += entry.weight;
    }
  }
  if (!right.length) right.push(left.pop());
  return [left, right];
}

async function writeSubset(io, source, entries, destination) {
  const document = await io.read(source);
  retainPrimitiveIndices(document, new Set(entries.map((entry) => entry.index)));
  stripUnlitNormals(document);
  await document.transform(dedup(), prune());
  configureDraco(document);
  await io.write(destination, document);
}

async function simplifyWholeGLB(io, source, destination, limit, overwrite) {
  const temporary = `${destination}.compressing.glb`;
  for (let index = 0; index < SIMPLIFY_RATIOS.length; index++) {
    const ratio = SIMPLIFY_RATIOS[index];
    const document = await io.read(source);
    stripUnlitNormals(document);
    await document.transform(
      weld({ tolerance: 1e-6 }),
      simplify({ simplifier: MeshoptSimplifier, ratio, error: 1 }),
      dedup(),
      prune(),
    );
    configureDraco(document);
    await fs.rm(temporary, { force: true });
    await io.write(temporary, document);
    const resultSize = (await fs.stat(temporary)).size;
    console.log(`  simplify pass ${index + 1}: ratio=${ratio}, size=${sizeText(resultSize)}`);
    if (resultSize <= limit) {
      if (overwrite) await fs.rm(destination, { force: true });
      await fs.rename(temporary, destination);
      return true;
    }
  }
  await fs.rm(temporary, { force: true });
  return false;
}

async function splitGLB(io, source, destination, limit, overwrite) {
  const sourceDocument = await io.read(source);
  const allEntries = listPrimitiveEntries(sourceDocument);
  if (!allEntries.length) throw new Error(`No mesh primitives found in ${source}`);

  const parsed = path.parse(destination);
  const finalParts = [];
  let candidateCounter = 0;

  async function emit(entries) {
    const candidate = path.join(parsed.dir,
      `${parsed.name}.candidate-${candidateCounter++}${parsed.ext || '.glb'}`);
    await writeSubset(io, source, entries, candidate);
    const candidateSize = (await fs.stat(candidate)).size;
    if (candidateSize <= limit) {
      const finalPath = path.join(parsed.dir,
        `${parsed.name}.part-${String(finalParts.length).padStart(3, '0')}${parsed.ext || '.glb'}`);
      if (overwrite) await fs.rm(finalPath, { force: true });
      await fs.rename(candidate, finalPath);
      finalParts.push(finalPath);
      console.log(`  split part ${finalParts.length}: primitives=${entries.length}, size=${sizeText(candidateSize)}`);
      return;
    }
    await fs.rm(candidate, { force: true });
    if (entries.length === 1) {
      throw new Error(`A single primitive is larger than ${sizeText(limit)}; cannot split safely.`);
    }
    const [left, right] = splitBalanced(entries);
    await emit(left);
    await emit(right);
  }

  // Remove stale parts only when explicitly allowed to overwrite.
  if (overwrite) {
    const siblings = await fs.readdir(parsed.dir).catch(() => []);
    for (const name of siblings) {
      if (name.startsWith(`${parsed.name}.part-`) && name.endsWith(parsed.ext || '.glb'))
        await fs.rm(path.join(parsed.dir, name), { force: true });
    }
    await fs.rm(destination, { force: true });
  }

  await emit(allEntries);
  return finalParts;
}

async function processLargeGLB(io, source, destination, options) {
  if (!options.overwrite && await exists(destination)) {
    const current = (await fs.stat(destination)).size;
    if (current <= options.limit) {
      console.log(`[skip existing] ${destination}`);
      return [destination];
    }
    throw new Error(`Output exists and is too large: ${destination} (use --overwrite)`);
  }

  await fs.mkdir(path.dirname(destination), { recursive: true });
  const originalSize = (await fs.stat(source)).size;
  console.log(`[process] ${source} (${sizeText(originalSize)})`);

  if (!options.splitOnly) {
    const simplified = await simplifyWholeGLB(
      io, source, destination, options.limit, options.overwrite);
    if (simplified) return [destination];
    console.log('  Whole-GLB simplification reached its safe structural limit; splitting without deleting primitives.');
  }
  return splitGLB(io, source, destination, options.limit, options.overwrite);
}

function relativeURI(fromJSON, target) {
  return path.relative(path.dirname(fromJSON), target).split(path.sep).join('/');
}

function patchTileContents(value, jsonPath, splitMap) {
  if (!value || typeof value !== 'object') return false;
  let changed = false;

  if (value.content?.uri && typeof value.content.uri === 'string') {
    const target = normalizedKey(path.resolve(path.dirname(jsonPath), value.content.uri));
    const parts = splitMap.get(target);
    if (parts?.length > 1) {
      const template = { ...value.content };
      delete template.uri;
      value.contents = parts.map((part) => ({ ...template, uri: relativeURI(jsonPath, part) }));
      delete value.content;
      changed = true;
    }
  }

  if (Array.isArray(value.contents)) {
    const expanded = [];
    for (const content of value.contents) {
      if (content?.uri && typeof content.uri === 'string') {
        const target = normalizedKey(path.resolve(path.dirname(jsonPath), content.uri));
        const parts = splitMap.get(target);
        if (parts?.length > 1) {
          const template = { ...content };
          delete template.uri;
          expanded.push(...parts.map((part) => ({ ...template, uri: relativeURI(jsonPath, part) })));
          changed = true;
          continue;
        }
      }
      expanded.push(content);
    }
    value.contents = expanded;
  }

  for (const child of Object.values(value)) {
    if (child && typeof child === 'object') changed = patchTileContents(child, jsonPath, splitMap) || changed;
  }
  return changed;
}

async function updateJSONReferences(jsonFiles, splitMap) {
  let changedCount = 0;
  for (const jsonPath of jsonFiles) {
    let data;
    try {
      data = JSON.parse(await fs.readFile(jsonPath, 'utf8'));
    } catch {
      continue;
    }
    if (patchTileContents(data, jsonPath, splitMap)) {
      await fs.writeFile(jsonPath, JSON.stringify(data));
      changedCount++;
      console.log(`[updated JSON] ${jsonPath}`);
    }
  }
  return changedCount;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (!options) {
    usage();
    return 0;
  }
  const inputStat = await fs.stat(options.input);
  if (inputStat.isDirectory() && pathIsInside(options.output, options.input)) {
    throw new Error('Output must not be inside input (recursive copy risk).');
  }

  const io = await createIO();
  const sourceFiles = inputStat.isFile() ? [options.input] : await collectFiles(options.input);
  // GLBs are processed first. JSON files are copied next and references patched last.
  sourceFiles.sort((a, b) => Number(path.extname(a).toLowerCase() !== '.glb') -
    Number(path.extname(b).toLowerCase() !== '.glb'));

  const splitMap = new Map();
  const outputJSON = [];
  let compressed = 0;
  let split = 0;
  let copied = 0;
  let failed = 0;

  for (const source of sourceFiles) {
    const relative = inputStat.isFile() ? path.basename(source) : path.relative(options.input, source);
    const destination = path.join(options.output, relative);
    const stat = await fs.stat(source);
    const extension = path.extname(source).toLowerCase();
    if (extension === '.glb' && stat.size > options.limit) {
      try {
        const outputs = await processLargeGLB(io, source, destination, options);
        if (outputs.length > 1) {
          splitMap.set(normalizedKey(destination), outputs);
          split++;
        } else {
          compressed++;
        }
      } catch (error) {
        console.error(`[failed] ${source}: ${error?.message ?? error}`);
        failed++;
      }
    } else if (options.copySmall) {
      try {
        await copyFile(source, destination, options.overwrite);
        if (extension === '.json') outputJSON.push(destination);
        copied++;
      } catch (error) {
        console.error(`[copy failed] ${source}: ${error?.message ?? error}`);
        failed++;
      }
    }
  }

  const updatedJSON = await updateJSONReferences(outputJSON, splitMap);
  if (splitMap.size) {
    const manifest = {};
    for (const [original, parts] of splitMap) manifest[original] = parts;
    await fs.mkdir(options.output, { recursive: true });
    await fs.writeFile(path.join(options.output, '_glb_split_manifest.json'), JSON.stringify(manifest, null, 2));
  }

  console.log(`\nDone: compressed=${compressed}, split=${split}, copied=${copied}, JSON-updated=${updatedJSON}, failed=${failed}, limit=${sizeText(options.limit)}`);
  console.log(`Output: ${options.output}`);
  return failed === 0 ? 0 : 1;
}

try {
  process.exitCode = await main();
} catch (error) {
  console.error(`Error: ${error?.message ?? error}`);
  process.exitCode = 2;
}
