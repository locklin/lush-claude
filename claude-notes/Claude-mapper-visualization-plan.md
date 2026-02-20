# Mapper Graph Visualization Tool: Architecture Plan

## Problem Statement

The current mapper visualization is a self-contained D3 HTML file. It works
well for quick inspection but has fundamental limitations:

- Each run produces a separate HTML file — no persistent workspace
- No way to compare graphs from different mapper runs side-by-side
- Running new mapper instances requires returning to the Lush REPL
- Data files accumulate in the working directory
- No project-level persistence across sessions

The goal is a thick-client visualization tool that supports interactive graph
exploration, node group selection with statistical testing, multi-graph
management, persistent project storage via SQLite, and the ability to launch
new mapper runs from the visualization tool itself.

---

## Architecture Options Compared

### Option A: Gephi Toolkit + Custom Java Swing/JavaFX UI

**Approach**: Use gephi-toolkit (0.10.1) as a library for graph data model,
layout algorithms, and metrics. Build a custom Swing or JavaFX UI on top,
with our own OpenGL or Java2D rendering for the interactive graph canvas.
SQLite for persistence.

**Pros**:
- Gephi-toolkit has mature graph algorithms (ForceAtlas2, modularity, etc.)
- Built-in SQLite import/export support (official demo exists)
- Java ecosystem — same language, single-process thick client
- Layout algorithms are battle-tested on large graphs
- Can use JGraphT alongside for additional graph algorithms

**Cons**:
- Gephi-toolkit does **not** include the visualization/UI modules — the
  rendering engine (OpenGL/JOGL/NetBeans Platform) is excluded from the
  toolkit JAR. You get graph model + layout + filters + I/O only.
- Must build the entire interactive graph canvas from scratch (node
  rendering, edge rendering, zoom/pan, lasso selection, hit testing)
- Gephi's visualization module is deeply coupled to NetBeans Platform and
  JOGL — extracting it is non-trivial and fragile
- JOGL dependency adds complexity (native libraries per platform)
- Java Swing is aging; JavaFX is better but adds another dependency
- Building a quality interactive graph renderer is a large project in itself

**Risk**: HIGH — the "hard part" (interactive graph visualization with lasso
selection) is exactly what the toolkit doesn't provide. We'd spend most of
our effort rebuilding what Gephi desktop already has.

**Reward**: MEDIUM — we'd get a pure-Java solution with good algorithms, but
the visualization would likely be inferior to existing tools for a long time.

---

### Option B: Cytoscape.js + Electron Desktop App

**Approach**: Build the thick client as an Electron app using Cytoscape.js
for graph visualization and better-sqlite3 for persistence. Lush communicates
with the app exclusively through the SQLite database.

**Pros**:
- Cytoscape.js is purpose-built for interactive graph visualization
- Built-in: zoom, pan, box selection, force-directed layouts, styling
- cytoscape-lasso plugin provides freeform lasso selection out of the box
- Mature ecosystem: used by Amazon, Google, IBM, Fujitsu
- Electron gives full filesystem and process access (can fork Lush)
- better-sqlite3 is synchronous and fast in Node.js
- HTML/CSS/JS makes UI development fast — tables, tabs, dropdowns are trivial
- Can reuse significant portions of our existing D3 visualization JS
  (statistical tests, comparison logic, table rendering)
- Rich extension ecosystem (layouts: cola, dagre, klay, fcose, etc.)
- Cross-platform (Linux, macOS, Windows) with single codebase
- Web technologies mean easy iteration on UI design

**Cons**:
- Electron apps are large (~150MB base) due to bundled Chromium
- Memory overhead per Chromium renderer process
- Node.js/JavaScript is a different language from Lush's C ecosystem
- Electron security model needs careful handling for process spawning
- Performance ceiling lower than native OpenGL for very large graphs
  (though Cytoscape.js handles thousands of nodes well)

**Risk**: LOW-MEDIUM — Cytoscape.js and Electron are well-proven for exactly
this use case. The SQLite communication pattern is simple and robust. Most of
the interactive visualization features we need exist as libraries.

**Reward**: HIGH — fast path to a working tool. Interactive graph viz with
lasso selection, tabbed multi-graph display, statistical comparison panels,
and process management are all straightforward in this stack.

---

### Option C: Gephi Desktop as External Process (Plugin Approach)

**Approach**: Use Gephi desktop application itself, writing a custom Gephi
plugin that monitors a SQLite database for new graphs and provides the
statistical testing UI. Lush writes graphs to SQLite; the plugin imports them.

**Pros**:
- Get Gephi's entire visualization engine for free (OpenGL, lasso, etc.)
- Plugin architecture is documented and maintained
- No need to build graph rendering from scratch
- Community-supported layout algorithms and visualization

**Cons**:
- Gephi plugin development is tied to NetBeans Platform (steep learning curve)
- Plugin API is not stable across versions
- Custom statistical testing UI must fit within Gephi's plugin framework
- Users must install Gephi separately (can't bundle easily)
- Limited control over the overall UX — constrained by Gephi's UI paradigm
- Gephi's plugin system is oriented toward general graph analysis, not our
  specific TDA workflow (group selection -> statistical testing -> re-run)
- Running mapper from within a Gephi plugin is awkward
- Gephi's SQLite support is import/export oriented, not live-monitoring

**Risk**: MEDIUM-HIGH — plugin development is fragile across Gephi versions,
and fitting our specific workflow into Gephi's UI paradigm may require
fighting the framework.

**Reward**: MEDIUM — good visualization for free, but limited customization
and awkward integration with the mapper workflow.

---

### Option D: Web App (Flask/FastAPI + Cytoscape.js) with SQLite

**Approach**: A local web server (Python or Node.js) that serves a browser-
based UI. Cytoscape.js for visualization. SQLite for persistence. Lush
communicates via SQLite; the web server polls for changes.

**Pros**:
- Same Cytoscape.js benefits as Option B
- No Electron overhead — uses the system browser
- Lighter deployment (no bundled Chromium)
- Python backend could use existing data science libraries

**Cons**:
- Browser security sandbox prevents direct process spawning (can't fork Lush
  from the browser; need the server as intermediary)
- More moving parts: browser + web server + SQLite + Lush
- Server process management adds complexity
- Harder to package as a self-contained application
- Browser tab can be accidentally closed; no system tray integration
- No native file dialogs without extra libraries

**Risk**: MEDIUM — more architectural pieces to coordinate, but each is
simple individually.

**Reward**: MEDIUM — lighter than Electron but more operationally complex.

---

### Option E: Pure Lush + X11 (Native Extension)

**Approach**: Build the visualization directly in Lush using its existing X11
graphics subsystem, with compiled C extensions for performance.

**Pros**:
- No external dependencies beyond X11
- Tight integration with Lush runtime
- Single process, simple communication

**Cons**:
- Lush's X11 driver is basic — no widgets, no text rendering beyond basics
- Must build everything from scratch: graph layout, node rendering, mouse
  interaction, lasso selection, scroll bars, tabs, text input, tables
- X11-only (no macOS without XQuartz, no native Wayland)
- This is essentially writing a GUI toolkit, which is years of work

**Risk**: VERY HIGH
**Reward**: LOW (tight integration) to VERY LOW (for the effort required)

---

## Recommendation

**Option B (Cytoscape.js + Electron)** is the clear winner on risk/reward.

| Criterion            | A (Gephi TK) | B (Electron) | C (Plugin) | D (Web App) | E (X11) |
|----------------------|:---:|:---:|:---:|:---:|:---:|
| Interactive graph viz | Must build | Built-in | Built-in | Built-in | Must build |
| Lasso selection      | Must build | Plugin | Built-in | Plugin | Must build |
| Statistical testing  | Must build | Reuse D3 JS | Must build | Reuse D3 JS | Must build |
| SQLite integration   | Good | Good | Limited | Good | Must build |
| Process management   | Manual | Native | Awkward | Via server | Native |
| Multi-graph tabs     | Must build | Trivial | Plugin | Trivial | Must build |
| Packaging            | JAR | Electron | Gephi install | Multiple | Single binary |
| Dev speed            | Slow | Fast | Medium | Medium | Very slow |
| Code reuse from D3   | None | High | None | High | None |
| Risk                 | High | Low-Med | Med-High | Medium | Very High |
| Reward               | Medium | High | Medium | Medium | Low |

---

## Detailed Design: Option B (Cytoscape.js + Electron)

### System Architecture

```
+------------------+          +-------------------+
|                  |          |                   |
|   Lush REPL      |  SQLite  |   Electron App    |
|   (mapper runs)  |<-------->|   (visualization) |
|                  |  .db     |                   |
+------------------+  file    +-------------------+
        |                            |
        |  fork                      |  fork
        v                            v
+------------------+          +------------------+
| Lush child       |          | Lush child       |
| (mapper run from |  write   | (mapper run from |
|  CLI)            |--------->|  viz tool)       |
+------------------+  to DB   +------------------+
```

All communication flows through a single SQLite database file. No sockets,
no IPC, no shared memory. SQLite's file-level locking handles concurrency.

### Component 1: packages/sqlite — Lush SQLite Binding

A new Lush package providing SQLite3 access from Lush. This is a general-
purpose package, not mapper-specific.

**Implementation approach**: Compile sqlite3.c amalgamation directly into a
Lush-loadable shared object, with thin C wrappers exposed to Lush via
`dhc-make`.

**API**:
```lisp
;; Open/close
(setq db (sqlite-open "/path/to/project.db"))
(sqlite-close db)

;; Execute (no results)
(sqlite-exec db "CREATE TABLE IF NOT EXISTS graphs (...)")

;; Query (returns list of rows, each row a list of values)
(sqlite-query db "SELECT id, name FROM graphs WHERE active = 1")

;; Parameterized queries (prevent SQL injection)
(sqlite-exec db "INSERT INTO nodes (graph_id, idx, size) VALUES (?, ?, ?)"
             graph-id node-idx node-size)

;; Transaction support
(sqlite-exec db "BEGIN TRANSACTION")
;; ... multiple operations ...
(sqlite-exec db "COMMIT")

;; Busy timeout (for concurrent access)
(sqlite-busy-timeout db 5000)  ; 5 second wait
```

**Files**:
```
packages/sqlite/
  sqlite.lsh            ; Lush API (open, close, exec, query)
  sqlite-c.c            ; C wrappers for dhc-make
  sqlite-c.h            ; Header
  sqlite-config.lsh     ; Compilation config
  sqlite3.c             ; SQLite amalgamation (public domain, ~250KB)
  sqlite3.h             ; SQLite header
```

**Why amalgamation**: SQLite is specifically designed to be embedded this way.
The amalgamation is a single C file containing the entire library. No external
dependency needed — it compiles everywhere Lush compiles.

### Component 2: Database Schema

A single `.db` file per project. All state lives here.

```sql
-- Project metadata
CREATE TABLE project (
    key TEXT PRIMARY KEY,
    value TEXT
);

-- Datasets (original data matrices)
CREATE TABLE datasets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    source_file TEXT,          -- original CSV path, if any
    n_rows INTEGER NOT NULL,
    n_cols INTEGER NOT NULL,
    col_names TEXT,            -- JSON array of column names
    created_at TEXT DEFAULT (datetime('now'))
);

-- Dataset values (row-major storage)
CREATE TABLE dataset_values (
    dataset_id INTEGER NOT NULL,
    row_idx INTEGER NOT NULL,
    col_idx INTEGER NOT NULL,
    value REAL NOT NULL,
    PRIMARY KEY (dataset_id, row_idx, col_idx),
    FOREIGN KEY (dataset_id) REFERENCES datasets(id)
);

-- Mapper run configurations
CREATE TABLE mapper_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    dataset_id INTEGER NOT NULL,
    name TEXT,
    -- Parameters
    n_cubes INTEGER NOT NULL,
    overlap REAL NOT NULL,
    metric TEXT NOT NULL,
    clusterer TEXT NOT NULL,
    eps REAL,
    min_pts INTEGER,
    min_intersection INTEGER DEFAULT 1,
    lens_type TEXT NOT NULL,       -- e.g., "l_inf_centrality"
    lens_metric TEXT,              -- metric used for lens, if different
    column_selection TEXT,         -- JSON: column indices or "variance:500"
    -- Status
    status TEXT DEFAULT 'pending', -- pending, running, completed, failed
    error_message TEXT,
    created_at TEXT DEFAULT (datetime('now')),
    completed_at TEXT,
    FOREIGN KEY (dataset_id) REFERENCES datasets(id)
);

-- Graph data (one per completed mapper run)
CREATE TABLE graphs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER NOT NULL UNIQUE,
    n_nodes INTEGER NOT NULL,
    n_edges INTEGER NOT NULL,
    FOREIGN KEY (run_id) REFERENCES mapper_runs(id)
);

-- Graph nodes
CREATE TABLE graph_nodes (
    graph_id INTEGER NOT NULL,
    node_idx INTEGER NOT NULL,
    size INTEGER NOT NULL,
    color REAL,
    bin_idx INTEGER,
    members TEXT NOT NULL,         -- JSON array of dataset row indices
    PRIMARY KEY (graph_id, node_idx),
    FOREIGN KEY (graph_id) REFERENCES graphs(id)
);

-- Graph edges
CREATE TABLE graph_edges (
    graph_id INTEGER NOT NULL,
    source INTEGER NOT NULL,
    target INTEGER NOT NULL,
    weight INTEGER NOT NULL,
    PRIMARY KEY (graph_id, source, target),
    FOREIGN KEY (graph_id) REFERENCES graphs(id)
);

-- User-defined node group labels
CREATE TABLE node_labels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    graph_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    color TEXT,                    -- hex color
    node_indices TEXT NOT NULL,    -- JSON array of node indices
    created_at TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (graph_id) REFERENCES graphs(id)
);

-- Active display state (what the viz tool should show)
CREATE TABLE display_state (
    graph_id INTEGER PRIMARY KEY,
    active INTEGER DEFAULT 1,     -- 1 = show in a tab, 0 = closed
    tab_order INTEGER,
    FOREIGN KEY (graph_id) REFERENCES graphs(id)
);
```

**Concurrency model**: Lush writes, Electron reads. For mapper runs launched
from the viz tool, a child Lush process writes. SQLite WAL mode allows
concurrent readers with one writer. The viz tool polls `display_state` and
`mapper_runs` every ~5 seconds.

### Component 3: Mapper SQLite Integration (packages/mapper changes)

Add functions to mapper to write results to SQLite instead of (or in addition
to) HTML:

```lisp
;; Write a dataset to the database
(mapper-db-store-dataset db name data col-names)

;; Write a completed mapper graph to the database
(mapper-db-store-graph db run-id graph data)

;; Store a graph and mark it for display
(mapper-db-store-and-display db run-id graph data)

;; Create a mapper run record (returns run-id)
(mapper-db-create-run db dataset-id params...)

;; Convenience: run mapper and store results in one call
(mapper-db-run db dataset-id ...)
```

### Component 4: Electron Visualization App

```
packages/mapper/viz/
  package.json
  main.js                 ; Electron main process
  preload.js              ; Bridge between main and renderer
  renderer/
    index.html            ; Main window
    app.js                ; Application logic
    graph-view.js         ; Cytoscape.js graph rendering
    stats-panel.js        ; Statistical comparison panel
    run-config.js         ; New mapper run configuration UI
    db.js                 ; SQLite database access
    styles.css
```

**Main Window Layout**:
```
+----------------------------------------------------------+
| [Tab: NKI run 1] [Tab: NKI run 2] [Tab: Iris] [+]       |
+----------------------------------------------------------+
| +------------------------------+ +-----------------------+|
| |                              | | Node Info / Stats     ||
| |   Cytoscape.js Graph         | |                       ||
| |   (force-directed layout)    | | Selected: 5 nodes     ||
| |                              | | Points: 142           ||
| |   [lasso select nodes]      | |                       ||
| |   [shift-click for groups]  | | Group A: blue (3)     ||
| |                              | | Group B: red (2)      ||
| |                              | +-----------------------+|
| |                              | | Comparison Results    ||
| |                              | | Test: Welch t-test    ||
| |                              | |                       ||
| |                              | | Gene1  p=0.001 d=2.3 ||
| |                              | | Gene2  p=0.003 d=1.8 ||
| |                              | | ...                   ||
| +------------------------------+ +-----------------------+|
+----------------------------------------------------------+
| [Color by: L-inf centrality v] [Layout: fcose v] [Run Mapper]|
+----------------------------------------------------------+
```

**Key Features**:

1. **Tabbed Multi-Graph Display**
   - Each tab is an independent Cytoscape.js instance
   - Tabs correspond to entries in `display_state` with `active=1`
   - Close tab -> set `active=0` in DB (graph still in DB, reopenable)
   - `[+]` button opens graph browser to reopen stored graphs

2. **Interactive Node Selection**
   - Click: select single node, show members in side panel
   - Lasso (shift-drag): freeform select multiple nodes
   - Box select (ctrl-drag): rectangle select
   - Assign selected nodes to Group A (blue) or Group B (red)
   - Groups stored in `node_labels` table for persistence

3. **Statistical Comparison Panel**
   - Reuse statistical test implementations from current D3 visualization
   - Welch t-test, Mann-Whitney U, KS test, hypergeometric test
   - Benjamini-Hochberg FDR correction
   - Sortable results table (by p-value, effect size, feature name)
   - Click feature row to re-color graph by that feature

4. **Data Export**
   - Export dataset with group labels as CSV
   - Export selected node members as a subset for re-analysis
   - Export graph as image (PNG/SVG)

5. **New Mapper Run from Viz Tool**
   - Configuration dialog with:
     - Dataset selection (from DB or load new CSV)
     - Metric selection (all options from metrics.lsh)
     - Lens selection (L-inf centrality, PCA, etc.)
     - Clustering method (slink / dbscan)
     - Parameters (n_cubes, overlap, eps, min_pts)
     - Column selection (top-N by variance, or manual selection)
   - On submit:
     1. Write `mapper_runs` record with status='pending'
     2. Fork a Lush child process:
        `lush -e '(progn (load "packages/mapper/mapper.lsh")
                         (mapper-db-run "/path/to/project.db" <run-id>))'`
     3. Child process: reads params from DB, runs mapper, writes graph to DB,
        sets status='completed', inserts into display_state, exits
     4. Viz tool polls `mapper_runs` and `display_state`, sees new graph, opens tab

6. **Database Polling Loop** (in main process)
   ```javascript
   setInterval(() => {
     // Check for new active graphs
     const active = db.prepare(
       `SELECT g.id, r.name FROM graphs g
        JOIN display_state d ON g.id = d.graph_id
        JOIN mapper_runs r ON g.run_id = r.id
        WHERE d.active = 1`
     ).all();
     // Compare with currently open tabs, open new ones
     // Check for completed mapper runs
     const completed = db.prepare(
       `SELECT id, name FROM mapper_runs
        WHERE status = 'completed' AND id NOT IN
        (SELECT run_id FROM graphs)`
     ).all();
     // ... handle new completions
   }, 5000);
   ```

### Component 5: Launcher Integration

From the Lush REPL:
```lisp
;; Start the visualization tool, pointing at a project database
(mapper-viz-start "/path/to/project.db")

;; This forks the Electron process and returns immediately
;; The Lush REPL stays active for further work
```

Implementation: `(sys "packages/mapper/viz/start.sh /path/to/project.db &")`

From the Electron app: the app can be launched standalone (double-click or
command line), opening a file dialog to select or create a project database.

---

## Implementation Phases

### Phase 1: SQLite Package (packages/sqlite)

Foundation that everything else depends on.

1. Download sqlite3 amalgamation (sqlite3.c + sqlite3.h)
2. Write C wrappers for: open, close, exec, prepare, step, bind, finalize
3. Write Lush API layer (sqlite.lsh)
4. Test: create DB, create table, insert, query, transactions
5. Test concurrent access (Lush writer + external reader)

### Phase 2: Mapper-SQLite Integration

Wire mapper output into SQLite.

1. Implement database schema creation
2. Implement dataset storage (matrix -> rows in dataset_values)
3. Implement graph storage (MapperGraph -> graph_nodes + graph_edges)
4. Implement `mapper-db-run` (read params from DB, run, write results)
5. Test: run demo-nki, store to DB, verify data integrity

### Phase 3: Electron App — Core Visualization

Get the basic graph display working.

1. Set up Electron project with Cytoscape.js and better-sqlite3
2. Implement DB reader (load graph from SQLite, convert to Cytoscape elements)
3. Implement graph rendering with node size/color mapping
4. Implement zoom, pan, and basic node selection
5. Implement polling loop for new graphs
6. Test: store graph from Lush, see it appear in Electron app

### Phase 4: Electron App — Interaction & Statistics

Add the analytical capabilities.

1. Implement lasso selection (cytoscape-lasso plugin)
2. Implement Group A/B assignment and persistence
3. Port statistical tests from visualize.lsh JavaScript
4. Implement comparison panel with sortable results table
5. Implement dynamic re-coloring by feature
6. Implement data export (CSV with labels, PNG/SVG graph)

### Phase 5: Electron App — Mapper Runner

Enable launching mapper from the viz tool.

1. Implement run configuration dialog
2. Implement dataset import from CSV (parse, store to DB)
3. Implement Lush process forking with correct arguments
4. Implement run status monitoring (poll mapper_runs.status)
5. Implement automatic tab opening on run completion
6. Test: configure run in UI, see graph appear when done

### Phase 6: Polish & Packaging

1. Graph browser (reopen closed graphs from DB)
2. Project metadata display
3. Column selection UI for datasets
4. Error handling and user feedback
5. Electron packaging (electron-builder for Linux/macOS)
6. Documentation

---

## Key Design Decisions

### Why SQLite for Communication (not sockets/pipes)?

1. **Persistence for free** — the communication channel IS the storage
2. **Crash-resilient** — if either process dies, no orphaned connections
3. **Debuggable** — can inspect state with `sqlite3` CLI at any time
4. **No protocol design** — schema IS the protocol
5. **Concurrency handled** — WAL mode handles reader/writer safely
6. **Language-agnostic** — C (Lush), JavaScript (Electron), Python all speak SQLite

### Why Electron over a Java thick client?

1. **Cytoscape.js** provides interactive graph visualization that we'd have to
   build from scratch in Java (gephi-toolkit excludes the rendering engine)
2. **Code reuse** — our existing D3 statistical tests port directly to JS
3. **Development speed** — HTML/CSS for UI is dramatically faster than Swing/JavaFX
4. **Packaging** — Electron apps are self-contained; Java needs a JRE

### Why not extend the existing D3 HTML approach?

The D3 HTML files are static snapshots. To add persistence, multi-graph
management, and process launching, we'd need a server anyway — at which point
we're building Option B or D. The current D3 approach remains valuable as a
quick-look export and should be preserved.

### Data storage: matrix in SQLite vs. blob?

The schema above stores matrix values as individual rows in `dataset_values`.
For a 272x500 matrix that's 136,000 rows — SQLite handles this trivially
(~5MB). This allows SQL queries over the data (e.g., "get column 3 for rows
in node 7"). For very large datasets (>100K rows x 10K cols), we could switch
to storing columns as BLOBs (packed float64 arrays), but this is a premature
optimization. Start with the row-per-value approach for simplicity.

---

## Dependencies

**Lush side**:
- sqlite3 amalgamation (public domain, vendored, ~250KB source)
- No new system dependencies

**Electron side**:
- Node.js (for development and electron-builder)
- Electron (~150MB bundled app size)
- cytoscape (~1MB)
- cytoscape-lasso (~10KB)
- cytoscape-fcose (layout, ~50KB)
- better-sqlite3 (native Node.js SQLite binding, fast, synchronous)
- Packaging: electron-builder

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| SQLite contention under heavy writes | WAL mode + busy timeout; only one writer at a time by design |
| Electron memory usage | One Cytoscape instance per active tab; lazy-load graph data |
| Large datasets slow to store/load | Batch INSERT with transactions; consider BLOB storage if needed |
| Lush child process failures | Write error to mapper_runs.error_message; viz tool shows error state |
| Cytoscape.js performance with very large graphs | Limit display to ~5000 nodes; offer filtering; use WebGL renderer extension if needed |
| Electron packaging complexity | Use electron-builder; test on Linux first (our primary platform) |
