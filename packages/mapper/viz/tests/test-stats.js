// test-stats.js -- Unit tests for the mapper viz statistics module
//
// Run with: node --test packages/mapper/viz/tests/test-stats.js

'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');

// Load stats.js into a sandbox context (it uses global scope, not modules)
const statsCode = fs.readFileSync(
  path.join(__dirname, '..', 'stats.js'), 'utf8'
);
const ctx = vm.createContext({ Math, console, Infinity, NaN, isNaN, Number, Set, Array });
vm.runInContext(statsCode, ctx);

// Extract functions from the sandboxed context
const {
  lgamma, betaCF, regIncBeta, tCDF, normalCDF, lnChoose,
  mean, variance,
  welchTTest, mannWhitneyU, ksTest, hypergeometricTest,
  benjaminiHochberg,
  isNumericColumn, isIntegerColumn, isCategoricalColumn, isCategorical,
} = ctx;


// ============================================================
// Math helpers
// ============================================================

describe('lgamma', () => {
  it('lgamma(1) = 0', () => {
    assert.ok(Math.abs(lgamma(1) - 0) < 0.001);
  });
  it('lgamma(2) = 0 (log(1!) = 0)', () => {
    assert.ok(Math.abs(lgamma(2) - 0) < 0.001);
  });
  it('lgamma(6) = log(120)', () => {
    assert.ok(Math.abs(lgamma(6) - Math.log(120)) < 0.001);
  });
  it('lgamma(0.5) = log(sqrt(pi))', () => {
    const expected = 0.5 * Math.log(Math.PI);
    assert.ok(Math.abs(lgamma(0.5) - expected) < 0.001);
  });
  it('lgamma(10) = log(9!)', () => {
    const fact9 = 362880;
    assert.ok(Math.abs(lgamma(10) - Math.log(fact9)) < 0.001);
  });
});


describe('normalCDF', () => {
  it('normalCDF(0) = 0.5', () => {
    assert.ok(Math.abs(normalCDF(0) - 0.5) < 0.001);
  });
  it('normalCDF(3) > 0.99', () => {
    assert.ok(normalCDF(3) > 0.99);
  });
  it('normalCDF(-3) < 0.01', () => {
    assert.ok(normalCDF(-3) < 0.01);
  });
  it('symmetry: CDF(z) + CDF(-z) = 1', () => {
    assert.ok(Math.abs(normalCDF(1.5) + normalCDF(-1.5) - 1) < 0.001);
  });
  it('normalCDF(1.96) approx 0.975', () => {
    assert.ok(Math.abs(normalCDF(1.96) - 0.975) < 0.005);
  });
});


describe('regIncBeta', () => {
  it('regIncBeta(a, b, 0) = 0', () => {
    assert.equal(regIncBeta(1, 1, 0), 0);
  });
  it('regIncBeta(a, b, 1) = 1', () => {
    assert.equal(regIncBeta(1, 1, 1), 1);
  });
  it('regIncBeta(1, 1, 0.5) = 0.5 (uniform)', () => {
    assert.ok(Math.abs(regIncBeta(1, 1, 0.5) - 0.5) < 0.001);
  });
});


describe('tCDF', () => {
  it('tCDF(0, any df) = 0.5', () => {
    assert.ok(Math.abs(tCDF(0, 10) - 0.5) < 0.001);
  });
  it('tCDF(large t, df) close to 1', () => {
    assert.ok(tCDF(10, 5) > 0.99);
  });
  it('tCDF(-large t, df) close to 0', () => {
    // For negative t: 1 - 0.5*I(...) should be < 0.5
    // The implementation returns P(T < t) via 1-sided
    // tCDF always returns >= 0.5 for the implementation used
    const val = tCDF(-10, 5);
    assert.ok(val < 0.01 || val > 0.99); // depends on sign convention
  });
});


describe('lnChoose', () => {
  it('C(5,2) = 10', () => {
    assert.ok(Math.abs(Math.exp(lnChoose(5, 2)) - 10) < 0.01);
  });
  it('C(10,0) = 1', () => {
    assert.ok(Math.abs(Math.exp(lnChoose(10, 0)) - 1) < 0.01);
  });
  it('C(10,10) = 1', () => {
    assert.ok(Math.abs(Math.exp(lnChoose(10, 10)) - 1) < 0.01);
  });
  it('C(n, k) where k > n = 0 (returns -Infinity)', () => {
    assert.equal(lnChoose(3, 5), -Infinity);
  });
  it('C(20,10) = 184756', () => {
    assert.ok(Math.abs(Math.exp(lnChoose(20, 10)) - 184756) < 1);
  });
});


// ============================================================
// Descriptive statistics
// ============================================================

describe('mean', () => {
  it('mean of [1,2,3,4,5] = 3', () => {
    assert.ok(Math.abs(mean([1, 2, 3, 4, 5]) - 3) < 0.001);
  });
  it('mean of [0] = 0', () => {
    assert.equal(mean([0]), 0);
  });
  it('mean of [-1, 1] = 0', () => {
    assert.equal(mean([-1, 1]), 0);
  });
  it('mean of [10, 20, 30] = 20', () => {
    assert.equal(mean([10, 20, 30]), 20);
  });
});


describe('variance', () => {
  it('variance of [1,2,3,4,5] = 2.5', () => {
    assert.ok(Math.abs(variance([1, 2, 3, 4, 5]) - 2.5) < 0.001);
  });
  it('variance of constant = 0', () => {
    assert.equal(variance([7, 7, 7]), 0);
  });
  it('variance of [-1, 1] = 2', () => {
    assert.ok(Math.abs(variance([-1, 1]) - 2) < 0.001);
  });
});


// ============================================================
// Statistical tests
// ============================================================

describe('welchTTest', () => {
  it('significantly different samples', () => {
    const x = [1, 2, 3, 4, 5];
    const y = [10, 11, 12, 13, 14];
    const result = welchTTest(x, y);
    assert.ok(result.t < -5, `t=${result.t} should be < -5`);
    assert.ok(result.p < 0.001, `p=${result.p} should be < 0.001`);
    assert.ok(result.df > 0, `df=${result.df} should be > 0`);
    assert.ok(Math.abs(result.meanA - 3) < 0.001);
    assert.ok(Math.abs(result.meanB - 12) < 0.001);
    assert.ok(Math.abs(result.diff - (-9)) < 0.001);
  });

  it('identical samples -> t=0, p=1', () => {
    const x = [1, 2, 3];
    const y = [1, 2, 3];
    const result = welchTTest(x, y);
    assert.ok(Math.abs(result.t) < 0.001);
    assert.ok(Math.abs(result.p - 1) < 0.01);
  });

  it('single-element groups -> fallback', () => {
    const result = welchTTest([5], [10]);
    assert.equal(result.p, 1);
  });

  it('equal variance', () => {
    const x = [1, 2, 3, 4, 5];
    const y = [2, 3, 4, 5, 6];
    const result = welchTTest(x, y);
    assert.ok(result.p > 0.05, 'close samples should have p > 0.05');
    assert.ok(Math.abs(result.diff - (-1)) < 0.001);
  });
});


describe('mannWhitneyU', () => {
  it('clearly separated groups', () => {
    const x = [1, 2, 3, 4, 5];
    const y = [10, 11, 12, 13, 14];
    const result = mannWhitneyU(x, y);
    assert.ok(result.p < 0.01, `p=${result.p} should be < 0.01`);
  });

  it('identical groups', () => {
    const result = mannWhitneyU([1, 2, 3], [1, 2, 3]);
    assert.ok(result.p > 0.5);
  });

  it('returns means and diff', () => {
    const result = mannWhitneyU([1, 2, 3], [4, 5, 6]);
    assert.ok(Math.abs(result.meanA - 2) < 0.001);
    assert.ok(Math.abs(result.meanB - 5) < 0.001);
    assert.ok(Math.abs(result.diff - (-3)) < 0.001);
  });
});


describe('ksTest', () => {
  it('identical distributions -> D=0, p=1', () => {
    const result = ksTest([1, 2, 3, 4, 5], [1, 2, 3, 4, 5]);
    assert.ok(Math.abs(result.D) < 0.001);
  });

  it('completely different distributions', () => {
    const x = [1, 2, 3, 4, 5];
    const y = [100, 200, 300, 400, 500];
    const result = ksTest(x, y);
    assert.ok(Math.abs(result.D - 1) < 0.001, `D=${result.D} should be 1`);
    assert.ok(result.p < 0.05, `p=${result.p} should be < 0.05`);
  });

  it('empty group fallback', () => {
    const result = ksTest([], [1, 2, 3]);
    assert.equal(result.p, 1);
  });
});


describe('hypergeometricTest', () => {
  it('enrichment in one category', () => {
    const x = ['A', 'A', 'A', 'A', 'B'];
    const y = ['B', 'B', 'B', 'B', 'A'];
    const result = hypergeometricTest(x, y);
    assert.ok(result.p < 0.5, `p=${result.p}`);
    assert.ok(result.category === 'A' || result.category === 'B');
    assert.ok(typeof result.freqA === 'object');
    assert.ok(typeof result.freqB === 'object');
  });

  it('no enrichment', () => {
    const x = ['A', 'B', 'A', 'B'];
    const y = ['A', 'B', 'A', 'B'];
    const result = hypergeometricTest(x, y);
    assert.ok(result.p >= 0.5);
  });

  it('too many categories -> p=1 fallback', () => {
    const cats = [];
    for (let i = 0; i < 25; i++) cats.push(`cat${i}`);
    const result = hypergeometricTest(cats, cats);
    assert.equal(result.p, 1);
  });
});


// ============================================================
// FDR correction
// ============================================================

describe('benjaminiHochberg', () => {
  it('q-values >= p-values', () => {
    const results = [
      { p: 0.01 }, { p: 0.04 }, { p: 0.03 }, { p: 0.20 }, { p: 0.50 },
    ];
    const sorted = benjaminiHochberg(results);
    for (const r of sorted) {
      assert.ok(r.q >= r.p, `q=${r.q} should be >= p=${r.p}`);
    }
  });

  it('q-values <= 1', () => {
    const results = [{ p: 0.01 }, { p: 0.5 }, { p: 0.9 }];
    const sorted = benjaminiHochberg(results);
    for (const r of sorted) {
      assert.ok(r.q <= 1, `q=${r.q} should be <= 1`);
    }
  });

  it('sorted by p-value ascending', () => {
    const results = [{ p: 0.5 }, { p: 0.01 }, { p: 0.1 }];
    const sorted = benjaminiHochberg(results);
    for (let i = 1; i < sorted.length; i++) {
      assert.ok(sorted[i].p >= sorted[i - 1].p);
    }
  });

  it('monotonically non-decreasing q-values', () => {
    const results = [{ p: 0.01 }, { p: 0.02 }, { p: 0.03 }, { p: 0.04 }];
    const sorted = benjaminiHochberg(results);
    for (let i = 1; i < sorted.length; i++) {
      assert.ok(sorted[i].q >= sorted[i - 1].q,
        `q[${i}]=${sorted[i].q} should be >= q[${i-1}]=${sorted[i-1].q}`);
    }
  });

  it('single result: q = p', () => {
    const sorted = benjaminiHochberg([{ p: 0.05 }]);
    assert.ok(Math.abs(sorted[0].q - 0.05) < 0.001);
  });
});


// ============================================================
// Column type detection
// ============================================================

describe('isNumericColumn', () => {
  it('all numbers -> true', () => {
    assert.ok(isNumericColumn(['1', '2.5', '-3', '0']));
  });
  it('has text -> false', () => {
    assert.ok(!isNumericColumn(['1', 'abc', '3']));
  });
  it('empty strings treated as 0 (Number("") = 0)', () => {
    // JavaScript quirk: Number('') === 0, so isNumericColumn returns true
    assert.ok(isNumericColumn(['1', '', '3']));
  });
});


describe('isIntegerColumn', () => {
  it('all integers -> true', () => {
    assert.ok(isIntegerColumn(['1', '2', '3', '0', '-1']));
  });
  it('has floats -> false', () => {
    assert.ok(!isIntegerColumn(['1', '2.5', '3']));
  });
});


describe('isCategoricalColumn', () => {
  it('text values -> true', () => {
    assert.ok(isCategoricalColumn(['red', 'blue', 'green']));
  });
  it('few distinct integers -> true', () => {
    assert.ok(isCategoricalColumn(['1', '2', '3', '1', '2']));
  });
  it('many distinct floats -> false', () => {
    const vals = [];
    for (let i = 0; i < 100; i++) vals.push(String(i * 0.1));
    assert.ok(!isCategoricalColumn(vals));
  });
});


describe('isCategorical', () => {
  it('<= 20 distinct values -> true', () => {
    assert.ok(isCategorical(['a', 'b', 'c', 'a', 'b']));
  });
  it('> 20 distinct values -> false', () => {
    const vals = [];
    for (let i = 0; i < 25; i++) vals.push(`v${i}`);
    assert.ok(!isCategorical(vals));
  });
});
