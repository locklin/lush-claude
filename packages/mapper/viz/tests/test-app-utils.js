// test-app-utils.js -- Unit tests for pure utility functions from app.js
//
// Run with: node --test packages/mapper/viz/tests/test-app-utils.js
//
// Tests the pure functions from app.js that don't require DOM or Cytoscape:
// color scales, data transforms, geometry (pointInPolygon), buildCyElements.

'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

// Load app.js into a sandbox with stubs for browser globals
const appCode = fs.readFileSync(
  path.join(__dirname, '..', 'app.js'), 'utf8'
);

// Track timers so we can clean up after app.js loads
const timers = [];

// Minimal stubs so app.js can load without errors
const ctx = vm.createContext({
  Math, console, JSON, parseInt, parseFloat, String, Number, Array, Object,
  Map, Set, Error, Promise, Infinity, NaN, isNaN, encodeURIComponent,
  setTimeout: (fn, ms) => { const id = setTimeout(fn, ms); timers.push(id); return id; },
  clearTimeout,
  setInterval: (fn, ms) => { const id = setInterval(fn, ms); timers.push(id); return id; },
  clearInterval,
  // Stub cytoscape and DOM
  cytoscape: { use: () => {} },
  cytoscapeFcose: undefined,
  cytoscapeCola: undefined,
  document: {
    getElementById: () => ({
      addEventListener: () => {},
      querySelector: () => null,
      querySelectorAll: () => [],
      style: {},
      innerHTML: '',
      textContent: '',
      value: '',
      classList: { add: () => {}, remove: () => {}, contains: () => false },
      appendChild: () => {},
      removeChild: () => {},
      children: [],
    }),
    querySelector: () => null,
    querySelectorAll: () => [],
    createElement: () => ({
      style: {},
      classList: { add: () => {}, remove: () => {} },
      addEventListener: () => {},
      appendChild: () => {},
    }),
  },
  // Return empty array for /api/graphs so pollForGraphs doesn't throw
  fetch: () => Promise.resolve({ ok: true, json: () => Promise.resolve([]) }),
  URL: { createObjectURL: () => '' },
  Blob: function() {},
  // Stats functions (loaded separately)
  lgamma: () => 0,
  normalCDF: () => 0.5,
  welchTTest: () => ({}),
  mannWhitneyU: () => ({}),
  ksTest: () => ({}),
  hypergeometricTest: () => ({}),
  benjaminiHochberg: (r) => r,
  mean: (arr) => arr.reduce((a, b) => a + b, 0) / arr.length,
  variance: () => 0,
  isCategorical: () => false,
  isCategoricalColumn: () => false,
  isNumericColumn: () => true,
});

vm.runInContext(appCode, ctx);

// Clean up polling timers started by app.js init
for (const id of timers) {
  clearTimeout(id);
  clearInterval(id);
}

// Extract testable functions
const {
  viridis, interpolateColor, colorForValue,
  pointInPolygon, buildCyElements,
} = ctx;


// ============================================================
// Color scales
// ============================================================

describe('viridis', () => {
  it('returns rgb string for t=0', () => {
    const c = viridis(0);
    assert.match(c, /^rgb\(\d+,\d+,\d+\)$/);
  });

  it('returns rgb string for t=1', () => {
    const c = viridis(1);
    assert.match(c, /^rgb\(\d+,\d+,\d+\)$/);
  });

  it('returns rgb string for t=0.5', () => {
    const c = viridis(0.5);
    assert.match(c, /^rgb\(\d+,\d+,\d+\)$/);
  });

  it('clamps t < 0 to 0', () => {
    assert.equal(viridis(-1), viridis(0));
  });

  it('clamps t > 1 to 1', () => {
    assert.equal(viridis(2), viridis(1));
  });

  it('different colors at different t values', () => {
    const c0 = viridis(0);
    const c1 = viridis(1);
    assert.notEqual(c0, c1);
  });

  it('rgb values are within 0-255', () => {
    for (let t = 0; t <= 1; t += 0.1) {
      const match = viridis(t).match(/rgb\((\d+),(\d+),(\d+)\)/);
      assert.ok(match, `viridis(${t}) should return valid rgb`);
      const [, r, g, b] = match.map(Number);
      assert.ok(r >= 0 && r <= 255, `r=${r} out of range at t=${t}`);
      assert.ok(g >= 0 && g <= 255, `g=${g} out of range at t=${t}`);
      assert.ok(b >= 0 && b <= 255, `b=${b} out of range at t=${t}`);
    }
  });
});


describe('interpolateColor', () => {
  it('returns rgb string', () => {
    assert.match(interpolateColor(0), /^rgb\(\d+,\d+,\d+\)$/);
  });

  it('clamps out of range', () => {
    assert.equal(interpolateColor(-0.5), interpolateColor(0));
    assert.equal(interpolateColor(1.5), interpolateColor(1));
  });

  it('produces different colors across range', () => {
    const colors = new Set();
    for (let t = 0; t <= 1; t += 0.1) {
      colors.add(interpolateColor(t));
    }
    assert.ok(colors.size >= 5, `expected >= 5 distinct colors, got ${colors.size}`);
  });

  it('transitions through 4 color stops', () => {
    // Each quartile should produce different colors
    const q0 = interpolateColor(0.125);
    const q1 = interpolateColor(0.375);
    const q2 = interpolateColor(0.625);
    const q3 = interpolateColor(0.875);
    const all = new Set([q0, q1, q2, q3]);
    assert.ok(all.size >= 3, 'should have distinct colors across quartiles');
  });
});


describe('colorForValue', () => {
  it('returns interpolated color', () => {
    const c = colorForValue(5, 0, 10);
    assert.match(c, /^rgb\(\d+,\d+,\d+\)$/);
  });

  it('min value maps to t=0', () => {
    assert.equal(colorForValue(0, 0, 10), interpolateColor(0));
  });

  it('max value maps to t=1', () => {
    assert.equal(colorForValue(10, 0, 10), interpolateColor(1));
  });

  it('mid value maps to t=0.5', () => {
    assert.equal(colorForValue(5, 0, 10), interpolateColor(0.5));
  });

  it('equal min/max returns mid color', () => {
    assert.equal(colorForValue(5, 5, 5), interpolateColor(0.5));
  });
});


// ============================================================
// Geometry
// ============================================================

describe('pointInPolygon', () => {
  const square = [
    { x: 0, y: 0 },
    { x: 10, y: 0 },
    { x: 10, y: 10 },
    { x: 0, y: 10 },
  ];

  it('point inside square', () => {
    assert.ok(pointInPolygon(5, 5, square));
  });

  it('point outside square', () => {
    assert.ok(!pointInPolygon(15, 5, square));
  });

  it('point far outside', () => {
    assert.ok(!pointInPolygon(-5, -5, square));
  });

  it('point above square', () => {
    assert.ok(!pointInPolygon(5, 15, square));
  });

  it('works with triangle', () => {
    const tri = [
      { x: 0, y: 0 },
      { x: 10, y: 0 },
      { x: 5, y: 10 },
    ];
    assert.ok(pointInPolygon(5, 3, tri));
    assert.ok(!pointInPolygon(0, 10, tri));
  });

  it('concave polygon', () => {
    // L-shaped polygon
    const lshape = [
      { x: 0, y: 0 },
      { x: 10, y: 0 },
      { x: 10, y: 5 },
      { x: 5, y: 5 },
      { x: 5, y: 10 },
      { x: 0, y: 10 },
    ];
    assert.ok(pointInPolygon(2, 2, lshape));    // in bottom
    assert.ok(pointInPolygon(2, 8, lshape));    // in left arm
    assert.ok(!pointInPolygon(8, 8, lshape));   // in concavity
  });

  it('empty polygon -> false', () => {
    assert.ok(!pointInPolygon(0, 0, []));
  });
});


// ============================================================
// Data transforms
// ============================================================

describe('buildCyElements', () => {
  it('converts graph data to cytoscape format', () => {
    const graphData = {
      nodes: [
        { id: 0, size: 10, color: 0.5, bin: 0, members: [0, 1, 2] },
        { id: 1, size: 5, color: 0.8, bin: 1, members: [3, 4] },
      ],
      edges: [
        { source: 0, target: 1, weight: 2 },
      ],
    };
    const elements = buildCyElements(graphData);
    assert.equal(elements.length, 3); // 2 nodes + 1 edge

    // Check node format
    const node0 = elements.find(e => e.data.id === 'n0');
    assert.ok(node0);
    assert.equal(node0.data.nodeIdx, 0);
    assert.equal(node0.data.size, 10);
    assert.equal(node0.data.color, 0.5);
    assert.deepEqual(node0.data.members, [0, 1, 2]);

    // Check edge format
    const edge0 = elements.find(e => e.data.id === 'e0');
    assert.ok(edge0);
    assert.equal(edge0.data.source, 'n0');
    assert.equal(edge0.data.target, 'n1');
    assert.equal(edge0.data.weight, 2);
  });

  it('handles empty edges', () => {
    const graphData = {
      nodes: [{ id: 0, size: 1, color: 0, bin: 0, members: [] }],
      edges: [],
    };
    const elements = buildCyElements(graphData);
    assert.equal(elements.length, 1);
  });

  it('handles missing edges property', () => {
    const graphData = {
      nodes: [{ id: 0, size: 1, color: 0, bin: 0, members: [] }],
    };
    const elements = buildCyElements(graphData);
    assert.equal(elements.length, 1);
  });

  it('node ids are prefixed with n', () => {
    const graphData = {
      nodes: [
        { id: 42, size: 1, color: 0, bin: 0, members: [] },
      ],
      edges: [],
    };
    const elements = buildCyElements(graphData);
    assert.equal(elements[0].data.id, 'n42');
  });

  it('edge ids are prefixed with e', () => {
    const graphData = {
      nodes: [
        { id: 0, size: 1, color: 0, bin: 0, members: [] },
        { id: 1, size: 1, color: 0, bin: 0, members: [] },
      ],
      edges: [
        { source: 0, target: 1, weight: 1 },
        { source: 1, target: 0, weight: 1 },
      ],
    };
    const elements = buildCyElements(graphData);
    const edges = elements.filter(e => e.data.id.startsWith('e'));
    assert.equal(edges.length, 2);
    assert.equal(edges[0].data.id, 'e0');
    assert.equal(edges[1].data.id, 'e1');
  });
});
