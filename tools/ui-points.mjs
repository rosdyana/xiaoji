// Display points (540x960) of the product's UI, for the page drivers (tools/latency.mjs, tools/snapshot-web.mjs).
// The all-apps drawer lists the apps by name, four to a row, in its first row at y 201 (docs/boot.md, B18: Settings'
// bounds [141,131][270,311] before Calculator). Which apps there are depends on the tree: the demo product
// (docs/demo.md) has Calculator, Files, Settings; the product before it had Files, Settings.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

export function drawerLayout(xjDir) {
  const m = JSON.parse(readFileSync(join(xjDir, 'manifest-xiaoji-v2.json'), 'utf8'));
  const calc = m.entries.some((e) => e.path.startsWith('/system_ext/app/ExactCalculator'));
  return calc
    ? { name: 'demo', calculator: [70, 201], files: [205, 201], settings: [340, 201] }
    : { name: 'before', files: [70, 201], settings: [205, 201] };
}
