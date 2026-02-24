# Query Language Design Notes for ColumnarDB

Design notes for building a SQL-like query sublanguage for the Lush columnardb
package.  Draws on Kerf1's approach and Lush's macro system (`dmd`).

---

## 1. Kerf1's Approach

Kerf1 bakes query syntax directly into the core language.  Verbs like `select`,
`from`, `where`, `group by`, and `order by` are first-class constructs handled
by the parser.  Kerf's parser supports both s-expression and infix SQL-like
syntax, so queries look natural:

```
result: select price, symbol from trades where price > 100 order by time
```

This works in Kerf because its reader/parser is custom-built to recognize
SQL-like infix syntax and rewrite it into internal verb dispatch.

**Why this is a non-starter for Lush:**  Lush's reader is s-expression based.
It reads balanced parenthesized forms, symbols, numbers, and strings.  There is
no infix operator support in the reader, and modifying the reader to handle
SQL-like syntax would break the simplicity of the Lisp model.  We need a
different approach that works within s-expression syntax.

---

## 2. Lush Macro Approach (dmd)

Lush has `dmd` (define macro dynamically), which rewrites s-expressions at
read/eval time.  We can use `dmd` to create a query sublanguage that *looks*
declarative but expands to imperative method calls on DataTable and ColumnarDB.

### Target Syntax

```lisp
(db-select ("price" "symbol")
  (from db "trades")
  (where (> "price" 100))
  (order-by "time")
  (limit 100))
```

Each clause is itself a macro or recognized keyword.  The top-level `db-select`
macro parses the clause list and emits the pipeline of calls.

### Expansion

The above would expand roughly to:

```lisp
(let ((dt (==> db table "trades")))
  (let ((filtered (==> dt where-rows '> "price" 100)))
    (let ((sorted (==> filtered sort-by "time" ())))
      (let ((limited (==> sorted select-rows (range* 0 100))))
        (==> limited select-columns (list "price" "symbol")) ) ) ) )
```

### Clause Macros

| Clause | Expansion target | Notes |
|--------|-----------------|-------|
| `(from db name)` | `(==> db table name)` | Source table |
| `(where (op col val))` | `(==> dt where-rows op col val)` | Filter rows |
| `(order-by col)` | `(==> dt sort-by col ())` | Sort ascending |
| `(order-by col 'desc)` | `(==> dt sort-by col t)` | Sort descending |
| `(limit n)` | `(==> dt select-rows (range* 0 n))` | Take first n |
| `(group-by col agg-list)` | `(columnardb-groupby dt col aggs)` | Aggregation |

### Multi-predicate WHERE

Compound predicates would use `and`/`or`:

```lisp
(where (and (> "price" 100) (= "symbol" "AAPL")))
```

This expands to chained filter calls or a single compiled scan that checks both
conditions per row.

---

## 3. State-Machine Query Planner

For more complex queries (especially those involving joins, multiple filters,
and aggregations), a pipeline-based query planner is the right architecture.

### Pipeline Stages

```
Scan -> Filter -> Project -> Sort -> Limit -> Materialize
```

Each stage reads from the previous stage.  The planner is free to reorder
stages for optimization.

### Stage Types

| Stage | Input | Output | Notes |
|-------|-------|--------|-------|
| **Scan** | Table path or DataTable | Row stream | Full scan or index scan |
| **Filter** | Row stream | Row stream | Predicate evaluation |
| **Project** | Row stream | Row stream | Column subset selection |
| **Sort** | Row stream | Row stream | Uses grade permutation |
| **Limit** | Row stream | Row stream | Truncate after N rows |
| **Group** | Row stream | Row stream | Aggregate by key columns |
| **Join** | Two row streams | Row stream | Hash join or sort-merge |

### Optimization Rules

1. **Push filter before sort** -- Reduce the number of rows to sort.
2. **Push filter before join** -- Filter each side independently.
3. **Use index when available** -- If a filter column has a CSR index or is
   sorted, use binary search or index lookup instead of full scan.
4. **Eliminate unused columns early** -- If only projecting 2 of 10 columns,
   don't load the other 8 (especially with mmap, this means those pages are
   never faulted in).
5. **Merge adjacent filters** -- Combine multiple filter stages into a single
   compiled scan loop.

### Plan Representation

A plan is a nested list (tree):

```lisp
'(limit 100
   (sort "time" asc
     (filter (> "price" 100)
       (scan "trades"))))
```

The executor walks the tree bottom-up, materializing each stage.

---

## 4. Implementation Roadmap

### Phase 1: Macro Sugar (Minimal)

- Implement `db-select` as a `dmd` macro.
- Support `from`, `where`, `order-by`, `limit` clauses.
- Expansion is direct: each clause becomes a method call.
- No optimizer, no planner -- just syntactic convenience.
- Depends only on existing DataTable methods and `Database.table`.

### Phase 2: Query Plan Objects

- Introduce a `QueryPlan` defclass that represents the pipeline as data.
- `db-select` expands to code that builds a plan, then executes it.
- The plan is inspectable: `(==> plan print-plan)` shows the stages.
- Simple rewrite rules (filter push-down, project push-down).

### Phase 3: Compiled Execution

- Write C primitives for hot-path operations:
  - Vectorized filter (evaluate predicate on entire column at once)
  - Vectorized aggregation (sum, count, min, max over column segment)
  - Hash-table probe for joins
- Use `dhc-make` to compile these into shared objects.
- The executor calls compiled primitives instead of Lush loops.

### Phase 4: Index-Aware Planning

- Planner checks `columnardb-meta` for sorted columns and CSR indexes.
- Sorted column + range filter => binary search scan.
- CSR index + equality filter => index lookup.
- Join on sorted columns => merge join.

### Phase 5: Multi-Table Queries

- JOIN support: `(join "trades" "instruments" ("symbol" "symbol"))`.
- Subquery support: nested `db-select` as a table source.
- The Database catalog provides the namespace for table resolution.

---

## 5. Functions and Primitives Needed

### Lush-Level Functions

| Function | Purpose |
|----------|---------|
| `db-select` (dmd) | Top-level query macro |
| `_query-parse-clauses` | Parse clause list into plan stages |
| `_query-optimize` | Apply rewrite rules to plan tree |
| `_query-execute` | Walk plan tree, materialize stages |
| `_query-filter-compile` | Turn predicate sexp into callable |
| `_query-join-hash` | Build hash table for hash-join |
| `_query-join-merge` | Merge-join two sorted streams |

### Compiled C Primitives (dhc-make)

| Primitive | Purpose |
|-----------|---------|
| `_cdb-vec-filter-gt-d` | Filter double column: col[i] > val |
| `_cdb-vec-filter-eq-i` | Filter int column: col[i] == val |
| `_cdb-vec-filter-range-d` | Filter double column: lo <= col[i] <= hi |
| `_cdb-vec-sum-d` | Sum a double column (with NaN skip) |
| `_cdb-vec-count-nonnull-d` | Count non-NaN doubles |
| `_cdb-vec-min-d` / `_cdb-vec-max-d` | Min/max with NaN handling |
| `_cdb-hash-probe-i` | Probe int hash table (for joins) |
| `_cdb-hash-probe-s` | Probe string hash table (for joins) |
| `_cdb-vec-gather` | Gather rows by index permutation (SIMD) |

### Existing Functions to Reuse

| Function | File | Reuse For |
|----------|------|-----------|
| `columnardb-range` | columnardb.lsh | Range scan on sorted columns |
| `columnardb-groupby` | columnardb-groupby.lsh | Group-by aggregation |
| `DataTable.where-rows` | datatable.lsh | Simple predicate filter |
| `DataTable.sort-by` | datatable.lsh | Sort by column |
| `DataTable.select-columns` | datatable.lsh | Column projection |
| `DataTable.select-rows` | datatable.lsh | Row selection by indices |
| `columnardb-meta` | columnardb.lsh | Read metadata for planning |

---

## 6. Open Questions

- **String predicate semantics**: Should `(where (= "symbol" "AAPL"))` use the
  CSR index automatically, or should the user explicitly request index usage?
  Recommendation: automatic -- the planner checks for index presence.

- **Lazy vs eager materialization**: Should each stage fully materialize its
  output as a new DataTable, or should stages pass row-index vectors?  Row-index
  passing avoids copying data until the final materialization, but requires all
  source columns to remain accessible.  For mmap'd tables this is essentially
  free.

- **NULL handling in predicates**: Lush uses NaN for double nulls and INT_MIN
  for int nulls.  Should `(where (> "price" 100))` skip NaN rows (SQL
  three-valued logic) or include them?  Recommendation: skip NaN/null (match
  SQL semantics).

- **Expression columns**: Should `db-select` support computed columns like
  `(as (* "price" "qty") "notional")`?  This would require evaluating
  expressions per row.  Phase 2+ feature.
