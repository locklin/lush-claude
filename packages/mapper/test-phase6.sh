#!/bin/bash
# Phase 6 end-to-end test: NKI dataset + synthetic two-circles dataset
# Tests: CSV upload, mapper run via HTTP, background worker, graph polling
set -e

LUSH="$(cd "$(dirname "$0")/../../bin" && pwd)/lush"
PKGDIR="$(cd "$(dirname "$0")/.." && pwd)"
DB="/tmp/claude/test-phase6.db"
PORT=8766
URL="http://localhost:$PORT"

rm -f "$DB"
mkdir -p /tmp/claude

echo "=== Phase 6 E2E Test ==="
echo "  Lush: $LUSH"
echo "  DB:   $DB"
echo "  Port: $PORT"

# ---------------------------------------------------------------
# 1. Generate synthetic CSV: two Gaussian clusters in 3-D
# ---------------------------------------------------------------
python3 -c "
import random, math
random.seed(42)
print('x,y,z,label')
for i in range(120):
    cx,cy,cz,lab = (0,0,0,0) if i<60 else (5,5,5,1)
    print(f'{cx+random.gauss(0,0.8):.6f},{cy+random.gauss(0,0.8):.6f},{cz+random.gauss(0,0.8):.6f},{lab}')
" > /tmp/claude/circles.csv
echo "[ok] Generated /tmp/claude/circles.csv (120 rows)"

# ---------------------------------------------------------------
# 2. Write Lush bootstrap script
#    - Load NKI subset (top 200 genes), run mapper, store in DB
#    - Then start the viz server (blocks)
# ---------------------------------------------------------------
cat > /tmp/claude/test-phase6-boot.lsh << 'LUSHEOF'
(addpath "PKGDIR_PLACEHOLDER")
(libload "mapper/mapper-db")
(libload "mapper/mapper-viz")
(libload "mapper/csvread")
(libload "mapper/lens")

;; Step A: Load NKI into DB via the fast native path
(printf "Loading NKI dataset...\n")(flush)
(let* ((datafile (concat "PKGDIR_PLACEHOLDER" "/mapper/NKI/NKI.csv.gz"))
       (result (mapper-tsv-load datafile))
       (data (car result))
       (gene-names (cadr result))
       (nrows (idx-dim data 0))
       (ncols (idx-dim data 1)) )

  (printf "NKI: %d patients x %d genes\n" nrows ncols)(flush)

  ;; Compute per-gene variance and pick top 200
  (let ((variances (double-matrix ncols)))
    (for (j 0 (1- ncols))
      (let ((sum 0.0))
        (for (i 0 (1- nrows))
          (incr sum (data i j)) )
        (let ((mean (/ sum nrows))
              (ssq 0.0) )
          (for (i 0 (1- nrows))
            (let ((d (- (data i j) mean)))
              (incr ssq (* d d)) ) )
          (variances j (/ ssq nrows)) ) ) )

    ;; Sort indices by variance descending (simple selection sort on top-N)
    (let ((var-idx (int-matrix ncols))
          (ntop 200) )
      (for (i 0 (1- ncols)) (var-idx i i))
      (for (i 0 (1- ntop))
        (for (j (+ i 1) (1- ncols))
          (when (> (variances (var-idx j)) (variances (var-idx i)))
            (let ((tmp (var-idx i)))
              (var-idx i (var-idx j))
              (var-idx j tmp) ) ) ) )

      ;; Skip clinical columns (variance > 1.0)
      (let ((gene-start 0))
        (while (and (< gene-start ncols)
                    (> (variances (var-idx gene-start)) 1.0))
          (incr gene-start) )

        ;; Build submatrix
        (let ((subdata (double-matrix nrows ntop))
              (sub-names ()) )
          (for (k 0 (1- ntop))
            (let ((gi (var-idx (+ gene-start k))))
              (setq sub-names (nconc1 sub-names (nth gi gene-names)))
              (for (i 0 (1- nrows))
                (subdata i k (data i gi)) ) ) )

          ;; Open DB, run mapper, store
          (printf "Running Mapper on NKI (200 genes)...\n")(flush)
          (let ((db (mapper-db-open "DB_PLACEHOLDER")))
            (let ((lens (mapper-lens-linf-centrality subdata "cosine")))
              (mapper-db-run db subdata sub-names "NKI-200genes" lens 15 0.4
                             'metric "cosine"
                             'clusterer "slink"
                             'eps 0.5
                             'lens-type "l_inf_centrality"
                             'source-file "NKI.csv.gz") )
            (printf "NKI graph stored.\n")(flush)
            (mapper-db-close db) ) ) ) ) ) )

;; Step B: Start viz server (blocks)
(printf "Starting viz server on port PORT_PLACEHOLDER...\n")(flush)
(mapper-viz-start "DB_PLACEHOLDER" PORT_PLACEHOLDER)
LUSHEOF

# Patch placeholders
sed -i "s|PKGDIR_PLACEHOLDER|$PKGDIR|g" /tmp/claude/test-phase6-boot.lsh
sed -i "s|DB_PLACEHOLDER|$DB|g" /tmp/claude/test-phase6-boot.lsh
sed -i "s|PORT_PLACEHOLDER|$PORT|g" /tmp/claude/test-phase6-boot.lsh

echo "[ok] Generated boot script"

# ---------------------------------------------------------------
# 3. Start Lush server in background
# ---------------------------------------------------------------
$LUSH /tmp/claude/test-phase6-boot.lsh > /tmp/claude/test-phase6-server.log 2>&1 &
SERVER_PID=$!
echo "[ok] Server starting (pid=$SERVER_PID)"

# Wait for server to be ready
echo -n "  Waiting for server..."
for i in $(seq 1 120); do
    if curl -s "$URL/api/graphs" > /dev/null 2>&1; then
        echo " ready!"
        break
    fi
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo " FAILED (server died)"
        echo "--- Server log ---"
        cat /tmp/claude/test-phase6-server.log
        exit 1
    fi
    sleep 1
    echo -n "."
done

# Verify NKI graph is already there
NKI_GRAPHS=$(curl -s "$URL/api/graphs")
echo "[check] Graphs after NKI load: $NKI_GRAPHS"
NKI_COUNT=$(echo "$NKI_GRAPHS" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")
if [ "$NKI_COUNT" -lt 1 ]; then
    echo "FAIL: Expected at least 1 graph from NKI"
    kill $SERVER_PID 2>/dev/null; exit 1
fi
echo "[ok] NKI graph present ($NKI_COUNT graph(s))"

# ---------------------------------------------------------------
# 4. Upload synthetic CSV via HTTP
# ---------------------------------------------------------------
echo ""
echo "--- Uploading synthetic circles dataset ---"
UPLOAD_RESP=$(curl -s -X POST "$URL/api/dataset?name=circles" \
  -H "Content-Type: text/csv" \
  --data-binary @/tmp/claude/circles.csv)
echo "[resp] $UPLOAD_RESP"

DATASET_ID=$(echo "$UPLOAD_RESP" | python3 -c "import json,sys; print(json.load(sys.stdin)['id'])")
echo "[ok] Dataset uploaded, id=$DATASET_ID"

# Verify datasets list
DATASETS=$(curl -s "$URL/api/datasets")
echo "[check] Datasets: $DATASETS" | head -c 300
echo ""

# ---------------------------------------------------------------
# 5. Start mapper run on circles via HTTP
# ---------------------------------------------------------------
echo ""
echo "--- Starting mapper run on circles ---"
RUN_RESP=$(curl -s -X POST "$URL/api/run" \
  -d "dataset_id=$DATASET_ID&name=circles-test&n_cubes=8&overlap=0.4&metric=euclidean&clusterer=slink&eps=0.5&min_pts=3&min_intersection=1&lens_type=l_inf_centrality&color_col=3")
echo "[resp] $RUN_RESP"

RUN_ID=$(echo "$RUN_RESP" | python3 -c "import json,sys; print(json.load(sys.stdin)['run-id'])")
echo "[ok] Run started, id=$RUN_ID"

# ---------------------------------------------------------------
# 6. Poll for run completion
# ---------------------------------------------------------------
echo -n "  Polling run status..."
for i in $(seq 1 60); do
    STATUS=$(curl -s "$URL/api/run/$RUN_ID" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','unknown'))")
    if [ "$STATUS" = "completed" ]; then
        echo " completed!"
        break
    elif [ "$STATUS" = "failed" ]; then
        echo " FAILED"
        curl -s "$URL/api/run/$RUN_ID" | python3 -m json.tool
        echo "--- Worker log ---"
        cat /tmp/claude/mapper-worker-${RUN_ID}.log 2>/dev/null || echo "(no log)"
        kill $SERVER_PID 2>/dev/null; exit 1
    fi
    sleep 1
    echo -n "."
done

if [ "$STATUS" != "completed" ]; then
    echo " TIMEOUT (status=$STATUS)"
    echo "--- Worker log ---"
    cat /tmp/claude/mapper-worker-${RUN_ID}.log 2>/dev/null || echo "(no log)"
    kill $SERVER_PID 2>/dev/null; exit 1
fi

# ---------------------------------------------------------------
# 7. Verify both graphs are present
# ---------------------------------------------------------------
echo ""
echo "--- Final verification ---"
ALL_GRAPHS=$(curl -s "$URL/api/graphs")
TOTAL=$(echo "$ALL_GRAPHS" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")
echo "[check] Total graphs: $TOTAL"

if [ "$TOTAL" -ge 2 ]; then
    echo "[ok] Both graphs present"
else
    echo "FAIL: Expected 2 graphs, got $TOTAL"
    kill $SERVER_PID 2>/dev/null; exit 1
fi

# Check the circles graph has nodes/edges
CIRCLES_GID=$(echo "$ALL_GRAPHS" | python3 -c "import json,sys; gs=json.load(sys.stdin); print(max(g['id'] for g in gs))")
GRAPH_DATA=$(curl -s "$URL/api/graph/$CIRCLES_GID")
N_NODES=$(echo "$GRAPH_DATA" | python3 -c "import json,sys; print(json.load(sys.stdin)['n-nodes'])")
N_EDGES=$(echo "$GRAPH_DATA" | python3 -c "import json,sys; print(json.load(sys.stdin)['n-edges'])")
echo "[ok] Circles graph #$CIRCLES_GID: $N_NODES nodes, $N_EDGES edges"

# Check worker process exited
sleep 1
if [ -f "/tmp/claude/mapper-worker-${RUN_ID}.log" ]; then
    echo "[ok] Worker log exists: /tmp/claude/mapper-worker-${RUN_ID}.log"
fi

# Check runs list
RUNS=$(curl -s "$URL/api/runs")
echo "[check] Runs: $(echo "$RUNS" | python3 -c "import json,sys; rs=json.load(sys.stdin); print([(r['name'],r['status']) for r in rs])")"

echo ""
echo "=== ALL TESTS PASSED ==="
echo "  Server still running at $URL (pid=$SERVER_PID)"
echo "  Open browser: $URL"
echo "  Kill server:  kill $SERVER_PID"
