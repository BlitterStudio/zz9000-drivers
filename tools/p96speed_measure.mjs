// Parse the original P96Speed 1.2 text export; timings come from the Amiga,
// never from this host. Required: P96SPEED_SAMPLE. For candidates, set
// P96SPEED_REFERENCE to semicolon-separated baseline exports from the same
// machine/firmware/mode. Each command invocation consumes one distinct run.
import { readFileSync } from 'node:fs';

const operations = [
  'RectFill()', 'RectFill() Pattern', 'WritePixel()',
  'WriteChunkyPixels()', 'WritePixelArray8()', 'WritePixelLine8()',
  'DrawEllipse()', 'DrawCircle()', 'Draw()', 'Draw() Hor/Ver',
  'ScrollRaster() X', 'ScrollRaster() Y', 'PutText()',
  'BlitBitMap()', 'BlitBitMapRastPort()', 'BitMapScale()',
  'OpenWindow()', 'MoveWindow()', 'SizeWindow()', 'CON-Output',
  'ScreenToFront()',
];

function parseExport(path) {
  const text = readFileSync(path, 'latin1');
  const resolution = text.match(/\|\s*Resolution\.{2,}:?\s*(\d+)\s*x\s*(\d+)\s*x\s*(\d+)\s*\|/i)
    ?? text.match(/\|\s*ScreenMode:\s*(\d+)\s*x\s*(\d+)\s*x\s*(\d+)\s*\|/i);
  const length = text.match(/\|\s*Testlength\.*:?\s*(\d+)\s*\|/i);
  if (!resolution || !length) throw new Error(`${path}: missing mode or test length`);
  if (+resolution[1] !== 640 || +resolution[2] !== 480 || +resolution[3] !== 8 ||
      +length[1] !== +(process.env.P96SPEED_TEST_LENGTH ?? '13')) {
    throw new Error(`${path}: expected 640x480x8, test length ${process.env.P96SPEED_TEST_LENGTH ?? 13}`);
  }

  const rows = new Map();
  for (const line of text.split(/\r?\n/)) {
    const match = line.match(/^\|\s*(.+?)\.{2,}\s*(\d+)\s+op\/s\s*\|/i);
    if (!match) continue;
    const name = match[1].trim();
    if (rows.has(name)) throw new Error(`${path}: duplicate ${name}`);
    rows.set(name, +match[2]);
  }
  for (const name of operations) {
    if (!rows.has(name) || rows.get(name) <= 0)
      throw new Error(`${path}: missing or invalid ${name}`);
  }
  if (rows.size !== operations.length)
    throw new Error(`${path}: unexpected operation in export`);
  return rows;
}

const samplePath = process.env.P96SPEED_SAMPLE;
if (!samplePath) throw new Error('Set P96SPEED_SAMPLE to a new P96Speed text export');
const sample = parseExport(samplePath);
const referencePaths = process.env.P96SPEED_REFERENCE?.split(';').filter(Boolean) ?? [];
const references = referencePaths.map(parseExport);
function median(values) {
  const sorted = values.toSorted((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}
let floor = 1;
for (const name of operations) {
  if (name === 'Draw()' || references.length === 0) continue;
  const reference = median(references.map((rows) => rows.get(name)));
  floor = Math.min(floor, sample.get(name) / reference);
}
console.log(JSON.stringify({
  draw_ops_per_second: sample.get('Draw()'),
  draw_hv_ops_per_second: sample.get('Draw() Hor/Ver'),
  p96speed_valid: 1,
  other_ops_floor: floor,
  non_draw_min_ratio: floor,
}));
