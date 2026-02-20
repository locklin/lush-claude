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

### Option D: Lush HTTP Server + Cytoscape.js in Browser

**Approach**: Lush itself serves as the HTTP server, delivering a Cytoscape.js
single-page application to the system browser. SQLite for persistence. No
external server process — the Lush process that runs mapper also serves the
visualization UI.

Lush already has the required infrastructure:
- `socketaccept` — listens on a TCP port, accepts connections (IPv4/IPv6)
- `socketselect` — multiplexed I/O for handling concurrent connections
- `read-string`, `read8`, `write8`, `printf` — byte-level socket I/O
- `bin/lushsocket` — an existing socket server implementation (REPL over TCP)
  that demonstrates the pattern

The HTTP protocol layer needed is minimal: serve ~5 static files (HTML, JS,
CSS) and ~5 JSON API endpoints. HTTP/1.1 keep-alive is not needed; HTTP/1.0
close-after-response is sufficient. The server only ever talks to localhost.

**Architecture**:
```
+------------------------------------------+
|  Lush Process                            |
|                                          |
|  +--------------+   +----------------+  |     +-----------+
|  | mapper engine|   | HTTP server    |------->| Browser   |
|  | (runs TDA)   |   | (port 8765)   |<-------| (Cyto.js) |
|  +--------------+   +----------------+  |     +-----------+
|        |                  |              |
|        v                  v              |
|  +----------------------------------+   |
|  |         SQLite database          |   |
|  +----------------------------------+   |
+------------------------------------------+
```

**HTTP endpoints**:
```
GET  /                     -> index.html (Cytoscape.js SPA)
GET  /app.js               -> application JavaScript
GET  /styles.css           -> stylesheet
GET  /api/graphs           -> JSON list of graphs in DB
GET  /api/graph/:id        -> JSON graph data (nodes, edges, members)
GET  /api/dataset/:id      -> JSON dataset metadata + column names
GET  /api/dataset/:id/cols -> JSON column data for selected columns
POST /api/run              -> create mapper run (params in body)
GET  /api/run/:id/status   -> check run status
POST /api/label            -> save node group label
```

The Lush HTTP server would be implemented as a package (`packages/httpd/` or
integrated into the mapper package) with ~200-300 lines of Lush code. The
existing `lushsocket` pattern shows exactly how to structure it: `socketaccept`
to listen, `reading`/`writing` to handle request/response, with a dispatch
table mapping URL paths to handler functions.

For forking new mapper runs from the browser: the Lush HTTP server receives
the POST request, writes the run config to SQLite, and either:
(a) runs mapper synchronously in a background thread (Lush has no threads,
    so this blocks the server), or
(b) forks a child Lush process (via `sys`) that runs mapper independently.
Option (b) is correct — the child writes results to SQLite, the HTTP server
continues responding to browser polls.

**Pros**:
- Same Cytoscape.js benefits as Option B — all the interactive graph
  visualization, lasso selection (plugin), layouts, etc.
- **No external dependencies** beyond a web browser (which every system has)
- **No Electron** — no bundled Chromium, no 150MB app, no Node.js needed
- **No separate server process** — Lush IS the server
- **Single ecosystem** — everything lives in Lush packages; no polyglot build
- **Minimal HTTP layer** — serving localhost static files + JSON APIs is
  ~200 lines of Lush, not a framework
- **Reuses existing D3/JS code** from current visualize.lsh (statistical
  tests, comparison logic, table rendering)
- **Natural integration** — the mapper engine and the server are in the same
  process; running a new mapper job is just a function call (or fork)
- **Lighter weight** than any other option — no JVM, no Electron, no pip
- **SQLite communication** still works — the DB is the persistence layer;
  the HTTP server just reads it and serves JSON to the browser
- **Trivially launchable**: `(mapper-viz-start db-path)` opens a socket,
  opens the browser with `sys "xdg-open http://localhost:8765"`, done

**Cons**:
- Must implement HTTP request parsing in Lush (~100 lines for the subset
  we need: GET with path extraction, POST with body reading)
- Browser security sandbox: no direct filesystem access (but the Lush
  server handles all file operations, so this doesn't matter)
- Browser tab can be accidentally closed (but just reopen the URL)
- No native file dialogs (use an HTML file-upload input for CSV import;
  the Lush server saves the uploaded data)
- Lush's single-threaded nature means the HTTP server blocks during
  request handling — but our requests are fast (SQLite reads) and
  mapper runs are forked as child processes, so this is fine for a
  single-user localhost tool
- If the Lush process exits, the server goes away (but so does the
  mapper engine — this is expected behavior)

**Risk**: LOW — the HTTP subset needed is tiny (serve 5 static files, handle
5 REST endpoints on localhost). Lush's socket infrastructure is proven. The
browser-side code is the same Cytoscape.js that Option B uses.

**Reward**: HIGH — same visualization capabilities as Option B, but with
zero external dependencies and tight Lush integration. The simplest possible
operational model: start Lush, call a function, browser opens.

---

### Option D-alt: Web App (Flask/FastAPI + Cytoscape.js) with SQLite

**Approach**: A separate web server process (Python or Node.js) that serves
a browser-based UI. This is the "external server" variant of Option D.

**Pros**:
- Same Cytoscape.js benefits as Option B
- No Electron overhead — uses the system browser
- Lighter deployment (no bundled Chromium)
- Python backend could use existing data science libraries

**Cons**:
- Browser security sandbox prevents direct process spawning
- More moving parts: browser + web server + SQLite + Lush
- Server process management adds complexity (separate install, separate start)
- Harder to package as a self-contained application
- External dependency on Python/pip or Node.js/npm
- Browser tab can be accidentally closed; no system tray integration

**Risk**: MEDIUM — more architectural pieces to coordinate.

**Reward**: MEDIUM — lighter than Electron but more operationally complex
than the Lush-native Option D.

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

**Option D (Lush HTTP Server + Cytoscape.js)** is the recommended approach.

It provides the same browser-side visualization capabilities as Option B
(Cytoscape.js, lasso selection, statistical testing, tabbed multi-graph) but
with zero external dependencies and tight Lush integration. The HTTP server
layer is minimal (~200-300 lines of Lush) because we only need to serve
localhost with a handful of static files and JSON endpoints.

Option B (Electron) remains a solid fallback if the Lush HTTP server proves
insufficient for any reason, since the browser-side code is identical — the
only difference is what serves it.

| Criterion            | A (Gephi TK) | B (Electron) | C (Plugin) | D (Lush HTTP) | D-alt (Flask) | E (X11) |
|----------------------|:---:|:---:|:---:|:---:|:---:|:---:|
| Interactive graph viz | Must build | Built-in | Built-in | Built-in | Built-in | Must build |
| Lasso selection      | Must build | Plugin | Built-in | Plugin | Plugin | Must build |
| Statistical testing  | Must build | Reuse JS | Must build | Reuse JS | Reuse JS | Must build |
| SQLite integration   | Good | Good | Limited | Native | Good | Must build |
| Process management   | Manual | Native | Awkward | Native | Via server | Native |
| Multi-graph tabs     | Must build | Trivial | Plugin | Trivial | Trivial | Must build |
| External deps        | JVM | Electron/Node | Gephi | **None** | Python/Node | X11 |
| Packaging            | JAR | Electron | Gephi install | **Lush pkg** | Multiple | Single binary |
| Dev speed            | Slow | Fast | Medium | Fast | Medium | Very slow |
| Code reuse from D3   | None | High | None | High | High | None |
| Lush integration     | None | Fork | None | **Same process** | Fork | Native |
| Risk                 | High | Low-Med | Med-High | **Low** | Medium | Very High |
| Reward               | Medium | High | Medium | **High** | Medium | Low |

---

## Detailed Design: Option D (Lush HTTP Server + Cytoscape.js)

### System Architecture

```
+------------------------------------------------------+
|  Lush Process (main)                                 |
|                                                      |
|  +------------------+     +---------------------+   |
|  | Mapper Engine     |     | HTTP Server         |   |
|  | - mapper-run      |     | - socketaccept 8765 |   |
|  | - mapper-db-run   |     | - serves static JS  |   |
|  +--------+---------+     | - serves JSON APIs  |   |
|           |               +----------+----------+   |
|           v                          |              |
|  +-----------------------------------+---+          |
|  |         SQLite Database (.db)         |          |     +----------+
|  +---------------------------------------+          |     | Browser  |
|                                                     |<--->| Cyto.js  |
+-----------------------------------------------------+     +----------+
        |                                          HTTP
        |  fork (for long-running mapper jobs)  localhost:8765
        v
+------------------+
| Lush child       |
| (mapper run      |
|  writes to DB,   |
|  then exits)     |
+------------------+
```

The Lush process serves dual roles: mapper computation engine and HTTP server.
The browser talks to Lush over HTTP on localhost. SQLite is the persistence
layer — the HTTP server reads it to serve JSON, mapper writes results to it.

For quick mapper runs, the main Lush process can run mapper directly. For
longer runs (or runs triggered from the browser UI), a child Lush process is
forked so the HTTP server remains responsive.

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

**Concurrency model**: The main Lush process is the only writer for most
operations. For mapper runs triggered from the browser, a child Lush process
writes results. SQLite WAL mode allows the main process to keep reading while
a child writes. The browser polls the HTTP server's JSON endpoints, which in
turn query SQLite.

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

### Component 4: Lush HTTP Server (packages/httpd)

A minimal HTTP/1.0 server implemented in Lush, using the existing socket
primitives. This is a general-purpose package, reusable beyond mapper.

```
packages/httpd/
  httpd.lsh              ; HTTP server: listen, accept, parse, respond
  httpd-static.lsh       ; Static file serving (with MIME types)
  httpd-json.lsh         ; JSON serialization helpers for API responses
```

**Core implementation** (~200-300 lines of Lush):

```lisp
;; Start server on a port, with a route dispatch table
(de httpd-start (port routes)
  (let ((listen-sock (socketaccept port)))
    (when (not listen-sock)
      (error "httpd" "cannot bind port" port))
    (printf "Mapper visualization: http://localhost:%d\n" port)
    ;; Event loop: accept connection, handle request, close
    (while t
      (socketaccept listen-sock 'fin 'fout)
      (let* ((request (httpd-parse-request fin))
             (method  (car request))
             (path    (cadr request))
             (handler (httpd-find-route routes method path)))
        (if handler
            (handler fin fout request)
          (httpd-send-404 fout path)))
      ;; Close connection (HTTP/1.0 style)
      (close fin)
      (close fout))))

;; Parse an HTTP request line + headers
;; Returns (method path headers body)
(de httpd-parse-request (fin)
  (reading fin
    (let* ((line (read-string))           ; "GET /api/graphs HTTP/1.1"
           (parts (split-string line " "))
           (method (car parts))
           (path (cadr parts))
           (headers (httpd-read-headers))
           (body (when (= method "POST")
                   (httpd-read-body headers))))
      (list method path headers body))))

;; Send a JSON response
(de httpd-send-json (fout json-string)
  (writing fout
    (printf "HTTP/1.0 200 OK\r\n")
    (printf "Content-Type: application/json\r\n")
    (printf "Content-Length: %d\r\n" (len json-string))
    (printf "Access-Control-Allow-Origin: *\r\n")
    (printf "\r\n")
    (printf "%s" json-string)
    (flush)))

;; Send a static file
(de httpd-send-file (fout filepath mime-type)
  (let ((content (read-file filepath)))
    (writing fout
      (printf "HTTP/1.0 200 OK\r\n")
      (printf "Content-Type: %s\r\n" mime-type)
      (printf "Content-Length: %d\r\n" (len content))
      (printf "\r\n")
      (printf "%s" content)
      (flush))))
```

The server handles one request at a time. This is fine because:
- Only one client (the local browser) connects
- Requests are fast (SQLite reads, static file serves)
- Long-running mapper jobs are forked as child processes
- Browser uses `fetch()` with async/await, so it doesn't block on slow responses

### Component 5: Browser UI (static files served by Lush)

```
packages/mapper/viz/
  index.html            ; Main page (single-page application)
  app.js                ; Application logic, API client, tab management
  graph-view.js         ; Cytoscape.js graph rendering and interaction
  stats-panel.js        ; Statistical comparison panel (ported from D3 viz)
  run-config.js         ; New mapper run configuration dialog
  styles.css            ; Styling
  lib/
    cytoscape.min.js    ; Cytoscape.js (vendored, ~1MB)
    cytoscape-lasso.js  ; Lasso selection plugin (vendored, ~10KB)
    cytoscape-fcose.js  ; Force-directed layout plugin (vendored, ~50KB)
```

These files are served by the Lush HTTP server as static assets. No build
step, no npm, no bundler — just plain JS files loaded by the browser. The
Cytoscape.js library and plugins are vendored (checked into the repo) so
there are zero runtime dependencies beyond a web browser.

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

5. **New Mapper Run from Browser**
   - Configuration dialog with:
     - Dataset selection (from DB or upload new CSV)
     - Metric selection (all options from metrics.lsh)
     - Lens selection (L-inf centrality, PCA, etc.)
     - Clustering method (slink / dbscan)
     - Parameters (n_cubes, overlap, eps, min_pts)
     - Column selection (top-N by variance, or manual selection)
   - On submit:
     1. Browser POSTs to `/api/run` with parameters
     2. Lush HTTP handler writes `mapper_runs` record with status='pending'
     3. Lush forks a child process:
        `(sys (sprintf "lush -e '(progn (load ...) (mapper-db-run \"%s\" %d))' &"
              db-path run-id))`
     4. Child process: reads params from DB, runs mapper, writes graph to DB,
        sets status='completed', inserts into display_state, exits
     5. Browser polls `/api/run/:id/status` and `/api/graphs`, sees new graph,
        opens tab

6. **Browser Polling Loop**
   ```javascript
   setInterval(async () => {
     // Check for new active graphs
     const resp = await fetch('/api/graphs?active=1');
     const graphs = await resp.json();
     // Compare with currently open tabs, open new ones
     for (const g of graphs) {
       if (!openTabs.has(g.id)) openTab(g);
     }
     // Check status of pending mapper runs
     for (const runId of pendingRuns) {
       const status = await fetch(`/api/run/${runId}/status`);
       // ... handle completions and errors
     }
   }, 5000);
   ```

### Component 6: Launcher Integration

From the Lush REPL:
```lisp
;; Open a project and start the visualization server
(mapper-viz-start "/path/to/project.db")

;; This:
;; 1. Opens the SQLite database
;; 2. Starts the HTTP server on an available port (e.g., 8765)
;; 3. Opens the system browser: xdg-open http://localhost:8765
;; 4. Enters the HTTP server event loop
;;
;; The REPL is occupied while the server runs. Press Ctrl-C to stop.
;; Alternatively, run in background:

(mapper-viz-start-bg "/path/to/project.db")

;; This forks a child Lush process running the HTTP server,
;; returns immediately so the REPL stays available for interactive work.
;; The child Lush process has mapper loaded and serves the viz UI.
```

**Standalone launch** (no REPL):
```bash
$ lush -e '(progn (load "packages/mapper/mapper.lsh") \
                  (mapper-viz-start "/path/to/project.db"))'
```

This gives a clean operational model: one command starts everything.

---

## Implementation Phases

### Phase 1: SQLite Package (packages/sqlite)

Foundation that everything else depends on.

1. Download sqlite3 amalgamation (sqlite3.c + sqlite3.h)
2. Write C wrappers for: open, close, exec, prepare, step, bind, finalize
3. Write Lush API layer (sqlite.lsh)
4. Test: create DB, create table, insert, query, transactions
5. Test concurrent access (Lush writer + external reader)

### Phase 2: HTTP Server Package (packages/httpd)

The minimal HTTP server layer.

1. Implement HTTP request parsing (method, path, headers, body)
2. Implement response helpers (send-json, send-file, send-404)
3. Implement route dispatch table (method + path pattern -> handler)
4. Implement static file serving with MIME type detection
5. Implement JSON serialization helpers (Lush lists/values -> JSON strings)
6. Test: serve a static HTML page, serve a JSON endpoint, verify in browser

### Phase 3: Mapper-SQLite Integration

Wire mapper output into SQLite.

1. Implement database schema creation
2. Implement dataset storage (matrix -> rows in dataset_values)
3. Implement graph storage (MapperGraph -> graph_nodes + graph_edges)
4. Implement `mapper-db-run` (read params from DB, run, write results)
5. Test: run demo-nki, store to DB, verify data integrity

### Phase 4: Browser UI — Core Visualization

Get the basic graph display working in the browser.

1. Write index.html with Cytoscape.js (vendored, no build step)
2. Implement `/api/graphs` and `/api/graph/:id` Lush HTTP handlers
3. Implement graph rendering with node size/color mapping in Cytoscape.js
4. Implement zoom, pan, and basic node selection
5. Implement browser polling for new graphs
6. Wire up `mapper-viz-start` to open the browser
7. Test: run demo-nki, store to DB, call mapper-viz-start, see graph in browser

### Phase 5: Browser UI — Interaction & Statistics

Add the analytical capabilities.

1. Integrate cytoscape-lasso plugin for freeform node selection
2. Implement Group A/B assignment UI and persistence via `/api/label`
3. Port statistical tests from current visualize.lsh JavaScript
4. Implement comparison panel with sortable results table
5. Implement dynamic re-coloring by feature (via `/api/dataset/:id/cols`)
6. Implement data export (CSV with labels, PNG/SVG graph)

### Phase 6: Browser UI — Mapper Runner

Enable launching mapper from the browser.

1. Implement run configuration dialog (HTML form)
2. Implement CSV upload via HTML file input + POST to `/api/dataset`
3. Implement `/api/run` POST handler that forks a child Lush process
4. Implement `/api/run/:id/status` handler for monitoring
5. Implement automatic tab opening on run completion (browser polls)
6. Test: configure run in browser, see graph appear when done

### Phase 7: Polish

1. Graph browser dialog (reopen closed graphs from DB)
2. `mapper-viz-start-bg` for background server (returns REPL to user)
3. Column selection UI for datasets
4. Error handling and user feedback
5. Documentation

---

## Key Design Decisions

### Why Lush as the HTTP server (not Flask/Node/Electron)?

1. **Zero external dependencies** — no Python, no Node.js, no JVM, no npm
2. **Single ecosystem** — everything is Lush packages, one build system
3. **Natural integration** — mapper engine and server share the same process;
   the server can directly query the SQLite DB it just wrote to
4. **Lush already has sockets** — `socketaccept`, `socketselect`, `read-string`,
   `write8` are all built in; `bin/lushsocket` proves the pattern works
5. **The HTTP subset needed is tiny** — serve ~5 static files and ~5 JSON
   endpoints on localhost; no TLS, no authentication, no routing framework
6. **Operationally simple** — one command starts everything; no separate
   process to install, configure, or manage

### Why SQLite for persistence?

1. **Persistence for free** — project state survives process restarts
2. **Crash-resilient** — no orphaned connections or corrupted state
3. **Debuggable** — can inspect state with `sqlite3` CLI at any time
4. **Schema IS the protocol** — no separate serialization format to maintain
5. **Concurrency handled** — WAL mode allows concurrent readers with one writer
6. **Cross-process** — forked child Lush processes write results to the same DB

### Why Cytoscape.js in a browser (not D3 or native X11)?

1. **Purpose-built for graphs** — Cytoscape.js is a graph visualization
   library, not a general-purpose drawing library like D3
2. **Interactive features built in** — zoom, pan, box select, node/edge events
3. **Lasso plugin exists** — freeform selection is a one-line integration
4. **Layout algorithms** — fcose, cola, dagre, elk — all available as plugins
5. **Code reuse** — existing statistical test JS from visualize.lsh ports directly
6. **UI development speed** — HTML/CSS for tables, tabs, forms is trivial

### Why not extend the existing D3 HTML approach?

The D3 HTML files are static snapshots. To add persistence, multi-graph
management, and process launching, we need a server — which is what Option D
provides. The current D3 approach remains valuable as a quick-look export
and should be preserved alongside the new tool.

### Data storage: matrix in SQLite vs. blob?

The schema stores matrix values as individual rows in `dataset_values`.
For a 272x500 matrix that's 136,000 rows — SQLite handles this trivially
(~5MB). This allows SQL queries over the data (e.g., "get column 3 for rows
in node 7"). For very large datasets (>100K rows x 10K cols), we could switch
to storing columns as BLOBs (packed float64 arrays), but this is a premature
optimization. Start with the row-per-value approach for simplicity.

### Single-threaded HTTP server: is it fast enough?

Yes. The server handles one request at a time, which is fine because:
- Only one client connects (the local browser)
- Static file serves are instant (read file, write to socket)
- JSON API responses are fast SQLite reads (~1ms for typical queries)
- Long-running mapper jobs are forked as child processes, not run in-line
- The browser uses async `fetch()`, so it doesn't block waiting for the server
- A typical interaction involves ~1 request per second at most

If we ever needed concurrency (unlikely for a single-user tool), Lush's
`socketselect` could support a non-blocking event loop, but the simple
blocking model is correct for this use case.

---

## Dependencies

**Lush side (all vendored, zero system dependencies)**:
- sqlite3 amalgamation (public domain, ~250KB C source, ~7MB compiled)
- No new system packages to install

**Browser side (all vendored in packages/mapper/viz/lib/)**:
- cytoscape.min.js (~1MB, MIT license)
- cytoscape-lasso.js (~10KB, MIT license)
- cytoscape-fcose.js (~50KB, MIT license)
- No npm, no build step, no CDN — plain JS files served by Lush

**Runtime requirements**:
- A web browser (any modern browser: Firefox, Chrome, etc.)
- That's it.

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Lush HTTP parsing edge cases | Only handle GET and POST; only serve localhost; test with major browsers |
| SQLite contention under heavy writes | WAL mode + busy timeout; only one writer at a time by design |
| Large datasets slow to store/load | Batch INSERT with transactions; consider BLOB storage if needed |
| Lush child process failures | Write error to mapper_runs.error_message; browser shows error state |
| Cytoscape.js perf with very large graphs | Limit display to ~5000 nodes; offer filtering; use WebGL renderer extension if needed |
| Browser tab accidentally closed | Just reopen the URL; all state is in SQLite, nothing is lost |
| Lush server blocks during request | Keep handlers fast (SQLite reads); fork long-running jobs; use socketselect if ever needed |
