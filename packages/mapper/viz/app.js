// Mapper Visualization Tool - app.js
// Cytoscape.js-based graph visualization with multi-tab support

'use strict';

// Register cytoscape extensions
if (typeof cytoscapeFcose !== 'undefined') {
  cytoscape.use(cytoscapeFcose);
}
if (typeof cytoscapeCola !== 'undefined') {
  cytoscape.use(cytoscapeCola);
}

// ============================================================
// State
// ============================================================

const openTabs = new Map();   // graphId -> { cy, data, runInfo, groupA, groupB, datasetInfo, colNames }
const closedGraphIds = new Set();  // graphs the user has explicitly closed (prevents poller re-opening)
let activeTabId = null;
let activeGroup = 'A';
let pollInterval = null;
let activeColaLayout = null;   // track running cola layout for stop
let compResults = [];          // current comparison results (all, unfiltered)
let compSortKey = 'p';
let compSortAsc = true;
let compTitle = '';
let compNA = 0;
let compNB = 0;

// ============================================================
// API client
// ============================================================

async function apiGet(path) {
  const resp = await fetch(path);
  if (!resp.ok) throw new Error(`API error: ${resp.status}`);
  return resp.json();
}

async function apiPost(path, data) {
  // Send as form-encoded for simple Lush parsing
  const parts = [];
  for (const [k, v] of Object.entries(data)) {
    parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(String(v)));
  }
  const resp = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: parts.join('&'),
  });
  if (!resp.ok) throw new Error(`API error: ${resp.status}`);
  return resp.json();
}

// ============================================================
// Color scales
// ============================================================

function viridis(t) {
  // Attempt a reasonable viridis-like gradient
  t = Math.max(0, Math.min(1, t));
  const r = Math.round(255 * Math.max(0, Math.min(1, -0.7 + 4.3*t - 4.8*t*t + 2.2*t*t*t)));
  const g = Math.round(255 * Math.max(0, Math.min(1, -0.1 + 1.2*t - 0.15*t*t)));
  const b = Math.round(255 * Math.max(0, Math.min(1, 0.3 + 1.5*t - 2.7*t*t + 1.4*t*t*t)));
  return `rgb(${r},${g},${b})`;
}

function interpolateColor(t) {
  // Blue (cold, low) -> white (mid) -> Red (hot, high)
  t = Math.max(0, Math.min(1, t));
  let r, g, b;
  if (t < 0.5) {
    const s = t / 0.5;
    r = Math.round(59 + s * (255 - 59));
    g = Math.round(76 + s * (255 - 76));
    b = Math.round(192 + s * (255 - 192));
  } else {
    const s = (t - 0.5) / 0.5;
    r = Math.round(255);
    g = Math.round(255 - s * (255 - 65));
    b = Math.round(255 - s * (255 - 54));
  }
  return `rgb(${r},${g},${b})`;
}

function colorForValue(val, minVal, maxVal) {
  if (maxVal === minVal) return interpolateColor(0.5);
  const t = (val - minVal) / (maxVal - minVal);
  return interpolateColor(t);
}

// ============================================================
// Tab management
// ============================================================

function createTab(graphId, name) {
  const tabBar = document.getElementById('tab-bar');
  const addBtn = document.getElementById('tab-add');

  const tab = document.createElement('div');
  tab.className = 'tab';
  tab.dataset.graphId = graphId;
  tab.innerHTML = `<span class="tab-name">${name}</span><span class="close-btn" onclick="event.stopPropagation();closeTab(${graphId})">&times;</span>`;
  tab.onclick = () => switchTab(graphId);
  tabBar.insertBefore(tab, addBtn);
}

function switchTab(graphId) {
  if (activeTabId === graphId) return;

  // Deactivate old
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.cy-instance').forEach(el => el.style.display = 'none');

  // Activate new
  const tabEl = document.querySelector(`.tab[data-graph-id="${graphId}"]`);
  if (tabEl) tabEl.classList.add('active');

  const state = openTabs.get(graphId);
  if (state && state.containerEl) {
    state.containerEl.style.display = 'block';
  }

  activeTabId = graphId;
  document.getElementById('loading').style.display = 'none';

  updateSidePanel();
  updateToolbar();
}

function closeTab(graphId) {
  closedGraphIds.add(graphId);
  // Tell server to mark graph inactive (fire-and-forget)
  fetch(`/api/graph/${graphId}/display`, { method: 'POST', body: 'active=0',
    headers: {'Content-Type':'application/x-www-form-urlencoded'} }).catch(()=>{});

  const state = openTabs.get(graphId);
  if (state) {
    if (state.cy) state.cy.destroy();
    if (state.containerEl) state.containerEl.remove();
    openTabs.delete(graphId);
  }

  const tabEl = document.querySelector(`.tab[data-graph-id="${graphId}"]`);
  if (tabEl) tabEl.remove();

  if (activeTabId === graphId) {
    activeTabId = null;
    if (openTabs.size > 0) {
      switchTab(openTabs.keys().next().value);
    } else {
      document.getElementById('loading').style.display = 'block';
      updateSidePanel();
    }
  }
}

// ============================================================
// Graph loading and rendering
// ============================================================

async function openGraph(graphId) {
  closedGraphIds.delete(graphId);  // clear closed state on explicit open
  if (openTabs.has(graphId)) {
    switchTab(graphId);
    return;
  }

  setStatus('Loading graph...');

  try {
    const graphData = await apiGet(`/api/graph/${graphId}`);
    let runInfo = null;
    try {
      runInfo = await apiGet(`/api/run/${graphData['run-id']}`);
    } catch(e) { /* ok if no run info */ }

    let datasetInfo = null;
    let colNames = [];
    if (runInfo && runInfo['dataset-id']) {
      try {
        datasetInfo = await apiGet(`/api/dataset/${runInfo['dataset-id']}`);
        if (datasetInfo && datasetInfo['col-names']) {
          colNames = datasetInfo['col-names'];
          if (typeof colNames === 'string') colNames = JSON.parse(colNames);
        }
      } catch(e) { /* ok */ }
    }

    const name = (runInfo && runInfo.name) || `Graph ${graphId}`;

    // Parse members from JSON strings
    graphData.nodes.forEach(n => {
      if (typeof n.members === 'string') {
        n.members = JSON.parse(n.members);
      }
    });

    // Create container
    const container = document.createElement('div');
    container.className = 'cy-instance';
    container.id = `cy-${graphId}`;
    document.getElementById('graph-container').appendChild(container);

    // Build Cytoscape elements
    const elements = buildCyElements(graphData);

    // Compute color range
    const colors = graphData.nodes.map(n => n.color);
    const minColor = Math.min(...colors);
    const maxColor = Math.max(...colors);

    // Create Cytoscape instance
    const cy = cytoscape({
      container: container,
      elements: elements,
      style: buildCyStyle(minColor, maxColor),
      layout: { name: 'preset' },  // place nodes first, run layout after
      minZoom: 0.1,
      maxZoom: 10,
      boxSelectionEnabled: false,
      userPanningEnabled: true,
      selectionType: 'additive',
      autoungrabify: false,
    });

    // Run layout after cytoscape is fully ready
    cy.ready(() => {
      cy.layout({
        name: 'fcose',
        animate: 'end',
        animationDuration: 1200,
        animationEasing: 'ease-out',
        randomize: true,
        fit: true,
        padding: 30,
        idealEdgeLength: 80,
        nodeRepulsion: 6000,
        edgeElasticity: 0.45,
        nestingFactor: 0.1,
        gravity: 0.25,
        numIter: 2500,
      }).run();
    });

    // Store state
    const state = {
      cy, data: graphData, runInfo, containerEl: container,
      name, minColor, maxColor, datasetInfo, colNames,
      groupA: new Set(), groupB: new Set(),
      colorField: 'default',
    };
    openTabs.set(graphId, state);

    // Create tab
    createTab(graphId, name);

    // Setup event handlers
    setupCyEvents(cy, graphId);

    // Switch to it
    switchTab(graphId);
    setStatus('Graph loaded');
  } catch(e) {
    setStatus('Error: ' + e.message);
    console.error(e);
  }
}

function buildCyElements(graphData) {
  const nodes = graphData.nodes.map(n => ({
    data: {
      id: 'n' + n.id,
      nodeIdx: n.id,
      size: n.size,
      color: n.color,
      bin: n.bin,
      members: n.members,
    }
  }));

  const edges = (graphData.edges || []).map((e, i) => ({
    data: {
      id: 'e' + i,
      source: 'n' + e.source,
      target: 'n' + e.target,
      weight: e.weight,
    }
  }));

  return [...nodes, ...edges];
}

function buildCyStyle(minColor, maxColor) {
  return [
    {
      selector: 'node',
      style: {
        'width': 'data(size)',
        'height': 'data(size)',
        'background-color': function(ele) {
          return colorForValue(ele.data('color'), minColor, maxColor);
        },
        'border-width': 1,
        'border-color': '#555',
        'label': '',
      }
    },
    {
      selector: 'node:selected',
      style: {
        'border-width': 4,
        'border-color': '#ffff00',
        'overlay-color': '#ffff00',
        'overlay-padding': 4,
        'overlay-opacity': 0.2,
      }
    },
    {
      selector: 'node.groupA',
      style: {
        'border-width': 4,
        'border-color': '#ff4444',
        'overlay-color': '#ff4444',
        'overlay-padding': 3,
        'overlay-opacity': 0.15,
      }
    },
    {
      selector: 'node.groupB',
      style: {
        'border-width': 4,
        'border-color': '#4488ff',
        'overlay-color': '#4488ff',
        'overlay-padding': 3,
        'overlay-opacity': 0.15,
      }
    },
    {
      selector: 'edge',
      style: {
        'width': function(ele) { return Math.max(1, Math.min(6, ele.data('weight'))); },
        'line-color': '#556',
        'opacity': 0.6,
        'curve-style': 'bezier',
      }
    },
  ];
}

function setupCyEvents(cy, graphId) {
  const tooltip = document.getElementById('tooltip');

  // Size mapping: scale node sizes
  const sizes = cy.nodes().map(n => n.data('size'));
  const minSize = Math.min(...sizes);
  const maxSize = Math.max(...sizes);

  cy.nodes().forEach(node => {
    const s = node.data('size');
    const scaled = 10 + 30 * (maxSize > minSize ? (s - minSize) / (maxSize - minSize) : 0.5);
    node.style({ 'width': scaled, 'height': scaled });
  });

  // Tooltip on mouseover
  cy.on('mouseover', 'node', function(evt) {
    const node = evt.target;
    const d = node.data();
    tooltip.innerHTML = `Node ${d.nodeIdx}<br>Size: ${d.size} points<br>Color: ${d.color.toFixed(4)}`;
    tooltip.style.display = 'block';
    const pos = evt.renderedPosition || evt.position;
    const rect = cy.container().getBoundingClientRect();
    tooltip.style.left = (rect.left + pos.x + 15) + 'px';
    tooltip.style.top = (rect.top + pos.y - 15) + 'px';
  });

  cy.on('mouseout', 'node', function() {
    tooltip.style.display = 'none';
  });

  // Click: add to active group
  cy.on('tap', 'node', function(evt) {
    const node = evt.target;
    const nodeIdx = node.data('nodeIdx');
    const state = openTabs.get(graphId);
    if (!state) return;

    const grp = activeGroup === 'A' ? state.groupA : state.groupB;
    const other = activeGroup === 'A' ? state.groupB : state.groupA;

    if (evt.originalEvent.shiftKey) {
      if (grp.has(nodeIdx)) grp.delete(nodeIdx);
      else grp.add(nodeIdx);
    } else {
      grp.clear();
      grp.add(nodeIdx);
    }
    other.delete(nodeIdx);

    updateGroupClasses(cy, state);
    updateSidePanel();
  });

  // Click on background: clear selection
  cy.on('tap', function(evt) {
    if (evt.target === cy) {
      // Don't clear on background tap — let shift-click add
    }
  });

  // Right-click lasso selection
  setupLasso(cy, graphId);
}

function updateGroupClasses(cy, state) {
  cy.nodes().forEach(node => {
    const idx = node.data('nodeIdx');
    node.removeClass('groupA groupB');
    if (state.groupA.has(idx)) node.addClass('groupA');
    if (state.groupB.has(idx)) node.addClass('groupB');
  });
}

// ============================================================
// Side panel updates
// ============================================================

function updateSidePanel() {
  if (!activeTabId || !openTabs.has(activeTabId)) {
    document.getElementById('info-name').textContent = '--';
    document.getElementById('info-nodes').textContent = '--';
    document.getElementById('info-edges').textContent = '--';
    document.getElementById('info-metric').textContent = '--';
    document.getElementById('info-clusterer').textContent = '--';
    document.getElementById('info-cubes').textContent = '--';
    document.getElementById('info-overlap').textContent = '--';
    document.getElementById('info-lens').textContent = '--';
    document.getElementById('info-topo-cols').textContent = '--';
    document.getElementById('sel-summary').textContent = '';
    document.getElementById('selection-info').style.display = 'none';
    return;
  }

  const state = openTabs.get(activeTabId);
  const data = state.data;
  const run = state.runInfo;

  document.getElementById('info-name').textContent = state.name;
  document.getElementById('info-nodes').textContent = data['n-nodes'];
  document.getElementById('info-edges').textContent = data['n-edges'];

  if (run) {
    document.getElementById('info-metric').textContent = run.metric || '--';
    document.getElementById('info-clusterer').textContent = run.clusterer || '--';
    document.getElementById('info-cubes').textContent = run['n-cubes'] || '--';
    document.getElementById('info-overlap').textContent = run.overlap || '--';

    // Parse column-selection: may be a string or already-parsed object
    const colSelRaw = run['column-selection'] || '';
    let colSelObj = null;
    if (typeof colSelRaw === 'object' && colSelRaw !== null) {
      colSelObj = colSelRaw;
    } else if (typeof colSelRaw === 'string' && colSelRaw.startsWith('{')) {
      try { colSelObj = JSON.parse(colSelRaw); } catch(e) { /* ignore */ }
    }

    // Display lens info
    const lensEl = document.getElementById('info-lens');
    let lensDisplay = run['lens-type'] || '--';
    if (colSelObj && colSelObj.lens_type_2 && colSelObj.lens_type_2 !== 'none') {
      lensDisplay += ' + ' + colSelObj.lens_type_2 + ' (2D)';
    }
    lensEl.textContent = lensDisplay;

    // Display topology columns info
    const topoColsEl = document.getElementById('info-topo-cols');
    if (colSelObj) {
      const cs = colSelObj;
      if (cs.mode === 'all' || (cs.cols && cs.cols.length === 0 && cs.mode !== 'auto_variance')) {
        topoColsEl.textContent = 'All';
      } else if (cs.mode === 'auto_variance') {
        if (cs.cols && cs.cols.length > 0) {
          const names = cs.cols.map(i => (state.colNames && state.colNames[i]) || ('col ' + i));
          topoColsEl.textContent = cs.cols.length + ' cols (top variance): ' + (names.length <= 5 ? names.join(', ') : names.slice(0, 5).join(', ') + '...');
        } else {
          topoColsEl.textContent = 'Top ' + (cs.top_n || '?') + ' by variance';
        }
      } else if (cs.mode === 'manual' && cs.cols && cs.cols.length > 0) {
        const names = cs.cols.map(i => (state.colNames && state.colNames[i]) || ('col ' + i));
        topoColsEl.textContent = cs.cols.length + ' cols: ' + (names.length <= 5 ? names.join(', ') : names.slice(0, 5).join(', ') + '...');
      } else {
        topoColsEl.textContent = 'All';
      }
    } else if (colSelRaw) {
      // Old format: bare color_col number
      topoColsEl.textContent = 'All (legacy)';
    } else {
      topoColsEl.textContent = 'All';
    }
  }

  // Selection summary
  const nA = state.groupA.size;
  const nB = state.groupB.size;
  let selText = '';
  if (nA > 0 || nB > 0) {
    selText = `A: ${nA} nodes | B: ${nB} nodes`;
  }
  document.getElementById('sel-summary').textContent = selText;

  // Selected node details
  const selPanel = document.getElementById('selection-info');
  const selDetails = document.getElementById('selected-details');
  const memberList = document.getElementById('member-list');

  if (nA > 0 || nB > 0) {
    selPanel.style.display = 'block';
    let membersA = new Set(), membersB = new Set();
    data.nodes.forEach(n => {
      if (state.groupA.has(n.id)) n.members.forEach(m => membersA.add(m));
      if (state.groupB.has(n.id)) n.members.forEach(m => membersB.add(m));
    });

    const arrA = [...membersA].sort((a,b) => a-b);
    const arrB = [...membersB].sort((a,b) => a-b);

    selDetails.innerHTML = `Group A: ${nA} nodes, ${arrA.length} points<br>Group B: ${nB} nodes, ${arrB.length} points`;

    let memberText = '';
    if (arrA.length > 0) memberText += `A: ${arrA.join(', ')}\n`;
    if (arrB.length > 0) memberText += `B: ${arrB.join(', ')}`;
    memberList.textContent = memberText;
  } else {
    selPanel.style.display = 'none';
  }
}

// ============================================================
// Group selection
// ============================================================

function setActiveGroup(group) {
  activeGroup = group;
  document.getElementById('btn-group-a').classList.toggle('active', group === 'A');
  document.getElementById('btn-group-b').classList.toggle('active', group === 'B');
}

function clearSelection() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;
  state.groupA.clear();
  state.groupB.clear();
  updateGroupClasses(state.cy, state);
  updateSidePanel();
}

// ============================================================
// Lasso selection (right-click drag)
// ============================================================

function setupLasso(cy, graphId) {
  let lassoCanvas = null;
  let lassoCtx = null;
  let lassoPath = [];
  let lassoing = false;

  cy.on('cxttapstart', function(evt) {
    // Start lasso on right-click drag on background or node
    const container = cy.container();
    lassoCanvas = document.createElement('canvas');
    lassoCanvas.id = 'lasso-canvas';
    lassoCanvas.width = container.offsetWidth;
    lassoCanvas.height = container.offsetHeight;
    lassoCanvas.style.position = 'absolute';
    lassoCanvas.style.top = '0';
    lassoCanvas.style.left = '0';
    lassoCanvas.style.pointerEvents = 'none';
    lassoCanvas.style.zIndex = '50';
    container.appendChild(lassoCanvas);
    lassoCtx = lassoCanvas.getContext('2d');
    lassoPath = [];
    lassoing = true;

    const pos = evt.renderedPosition || evt.position;
    if (pos) lassoPath.push({ x: pos.x, y: pos.y });
  });

  cy.on('cxtdrag', function(evt) {
    if (!lassoing || !lassoCtx) return;
    const pos = evt.renderedPosition || evt.position;
    if (!pos) return;
    lassoPath.push({ x: pos.x, y: pos.y });

    // Draw lasso
    lassoCtx.clearRect(0, 0, lassoCanvas.width, lassoCanvas.height);
    lassoCtx.beginPath();
    lassoCtx.moveTo(lassoPath[0].x, lassoPath[0].y);
    for (let i = 1; i < lassoPath.length; i++) {
      lassoCtx.lineTo(lassoPath[i].x, lassoPath[i].y);
    }
    lassoCtx.closePath();
    lassoCtx.strokeStyle = 'rgba(255,255,0,0.8)';
    lassoCtx.lineWidth = 2;
    lassoCtx.stroke();
    lassoCtx.fillStyle = 'rgba(255,255,0,0.1)';
    lassoCtx.fill();
  });

  cy.on('cxttapend', function() {
    if (!lassoing) return;
    lassoing = false;

    // Find nodes inside the lasso polygon using ray casting
    if (lassoPath.length > 2) {
      const state = openTabs.get(graphId);
      if (state) {
        const grp = activeGroup === 'A' ? state.groupA : state.groupB;
        const other = activeGroup === 'A' ? state.groupB : state.groupA;
        let count = 0;

        cy.nodes().forEach(node => {
          const rpos = node.renderedPosition();
          if (pointInPolygon(rpos.x, rpos.y, lassoPath)) {
            const idx = node.data('nodeIdx');
            grp.add(idx);
            other.delete(idx);
            count++;
          }
        });

        if (count > 0) {
          updateGroupClasses(cy, state);
          updateSidePanel();
          setStatus(`Lasso selected ${count} nodes into Group ${activeGroup}`);
        }
      }
    }

    // Remove canvas overlay
    if (lassoCanvas && lassoCanvas.parentNode) {
      lassoCanvas.parentNode.removeChild(lassoCanvas);
    }
    lassoCanvas = null;
    lassoCtx = null;
    lassoPath = [];
  });
}

// Ray casting point-in-polygon test
function pointInPolygon(x, y, polygon) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const xi = polygon[i].x, yi = polygon[i].y;
    const xj = polygon[j].x, yj = polygon[j].y;
    const intersect = ((yi > y) !== (yj > y)) &&
      (x < (xj - xi) * (y - yi) / (yj - yi) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

// ============================================================
// Layout
// ============================================================

function runLayout() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  // Stop any running cola layout
  stopColaLayout();

  const layoutName = document.getElementById('layout-select').value;

  if (layoutName === 'cola') {
    // Cola interactive force layout: first run a finite pass to get
    // a good initial placement fitted to the viewport, then switch
    // to infinite mode for interactive dragging.
    const finiteLayout = state.cy.layout({
      name: 'cola',
      infinite: false,
      fit: true,
      padding: 30,
      animate: true,
      randomize: false,
      maxSimulationTime: 4000,
      nodeSpacing: 20,
      edgeLength: 80,
      handleDisconnected: true,
    });
    finiteLayout.one('layoutstop', function() {
      // Now start infinite simulation for interactive dragging
      const infiniteLayout = state.cy.layout({
        name: 'cola',
        infinite: true,
        fit: false,
        animate: true,
        randomize: false,
        maxSimulationTime: 0,
        nodeSpacing: 20,
        edgeLength: 80,
        convergenceThreshold: 0.001,
        handleDisconnected: true,
      });
      activeColaLayout = infiniteLayout;
      infiniteLayout.run();
      setStatus('Cola layout running (drag nodes to interact)');
    });
    finiteLayout.run();
    return;
  }

  const opts = {
    name: layoutName,
    animate: 'end',
    animationDuration: 1200,
    animationEasing: 'ease-out',
    fit: true,
    padding: 30,
  };
  if (layoutName === 'fcose') {
    opts.randomize = true;
    opts.idealEdgeLength = 80;
    opts.nodeRepulsion = 6000;
    opts.edgeElasticity = 0.45;
    opts.gravity = 0.25;
    opts.numIter = 2500;
  }
  state.cy.layout(opts).run();
}

function stopColaLayout() {
  if (activeColaLayout) {
    activeColaLayout.stop();
    activeColaLayout = null;
    setStatus('Layout stopped');
  }
}

// ============================================================
// Recolor
// ============================================================

async function recolor() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  const field = document.getElementById('color-select').value;
  state.colorField = field;

  if (field.startsWith('col:')) {
    await recolorByColumn(state, parseInt(field.split(':')[1]));
    return;
  }

  let values;
  if (field === 'size') {
    values = state.data.nodes.map(n => n.size);
  } else if (field === 'degree') {
    values = state.cy.nodes().map(n => n.degree());
  } else {
    values = state.data.nodes.map(n => n.color);
  }

  const minVal = Math.min(...values);
  const maxVal = Math.max(...values);

  state.cy.nodes().forEach((node, i) => {
    const val = field === 'degree' ? node.degree() : values[i];
    node.style('background-color', colorForValue(val, minVal, maxVal));
  });
}

async function recolorByColumn(state, colIdx) {
  setStatus('Fetching column data...');
  try {
    const did = state.runInfo && state.runInfo['dataset-id'];
    if (!did) { setStatus('No dataset'); return; }
    const colData = await apiGet(`/api/dataset/${did}/columns?cols=${colIdx}`);
    let values;
    if (Array.isArray(colData) && colData.length > 0) {
      values = colData[0];
    } else if (colData.columns && colData.columns.length > 0) {
      values = colData.columns[0].values || colData.columns[0];
    } else {
      setStatus('No column data'); return;
    }

    // Detect if column is string/categorical (non-numeric values)
    const isStringCol = values.some(v =>
      v !== null && v !== undefined && typeof v === 'string' && isNaN(Number(v))
    );

    let nodeValues;
    if (isStringCol) {
      // String/categorical column: assign numeric indices to unique values
      // and average those indices per node
      const uniqueVals = [...new Set(values.filter(v => v !== null && v !== undefined))].sort();
      const valMap = new Map();
      uniqueVals.forEach((v, i) => valMap.set(String(v), i));

      nodeValues = state.data.nodes.map(n => {
        const members = n.members || [];
        if (members.length === 0) return 0;
        let sum = 0;
        for (const m of members) {
          const v = values[m];
          sum += (valMap.has(String(v)) ? valMap.get(String(v)) : 0);
        }
        return sum / members.length;
      });
    } else {
      // Numeric column: compute mean value per node
      nodeValues = state.data.nodes.map(n => {
        const members = n.members || [];
        if (members.length === 0) return 0;
        let sum = 0;
        for (const m of members) {
          const v = values[m];
          sum += (v !== null && v !== undefined ? Number(v) : 0);
        }
        return sum / members.length;
      });
    }

    const minVal = Math.min(...nodeValues);
    const maxVal = Math.max(...nodeValues);

    state.cy.nodes().forEach((node, i) => {
      node.style('background-color', colorForValue(nodeValues[i], minVal, maxVal));
    });

    const colName = state.colNames[colIdx] || `col ${colIdx}`;
    setStatus('Colored by ' + colName + (isStringCol ? ' (categorical)' : ''));
  } catch(e) {
    setStatus('Error: ' + e.message);
  }
}

// ============================================================
// Toolbar
// ============================================================

function updateToolbar() {
  const sel = document.getElementById('color-select');
  // Remove old column options
  while (sel.options.length > 3) sel.remove(3);
  // Add column options from active tab's dataset
  if (activeTabId && openTabs.has(activeTabId)) {
    const state = openTabs.get(activeTabId);
    if (state.colNames && state.colNames.length > 0) {
      state.colNames.forEach((name, i) => {
        const opt = document.createElement('option');
        opt.value = 'col:' + i;
        opt.textContent = name;
        sel.appendChild(opt);
      });
    }
  }
  sel.value = 'default';
}

function setStatus(msg) {
  document.getElementById('status-bar').textContent = msg;
}

// ============================================================
// Graph picker
// ============================================================

async function showGraphPicker() {
  const modal = document.getElementById('graph-picker');
  const list = document.getElementById('graph-list');

  try {
    const graphs = await apiGet('/api/graphs');
    list.innerHTML = '';

    if (graphs.length === 0) {
      list.innerHTML = '<div style="color:#888;padding:8px">No graphs in database</div>';
    } else {
      graphs.forEach(g => {
        const item = document.createElement('div');
        item.className = 'graph-item';
        const isOpen = openTabs.has(g.id);
        item.innerHTML = `<span class="name">${g.name}${isOpen ? ' (open)' : ''}</span><span class="meta">${g['n-nodes']} nodes, ${g['n-edges']} edges</span>`;
        item.onclick = () => { hideGraphPicker(); openGraph(g.id); };
        list.appendChild(item);
      });
    }

    modal.classList.add('visible');
  } catch(e) {
    setStatus('Error loading graphs: ' + e.message);
  }
}

function hideGraphPicker() {
  document.getElementById('graph-picker').classList.remove('visible');
}

// ============================================================
// Polling
// ============================================================

async function pollForGraphs() {
  try {
    const graphs = await apiGet('/api/graphs');
    // Auto-open any graph that's active and not yet open (skip user-closed graphs)
    for (const g of graphs) {
      if (g.active === 1 && !openTabs.has(g.id) && !closedGraphIds.has(g.id)) {
        console.log('pollForGraphs: opening graph', g.id, g.name);
        await openGraph(g.id);
      }
    }
  } catch(e) {
    console.error('pollForGraphs error:', e);
  }
}

// ============================================================
// Group comparison
// ============================================================

function getMembersForGroup(state, group) {
  const members = new Set();
  state.data.nodes.forEach(n => {
    if (group.has(n.id)) {
      (n.members || []).forEach(m => members.add(m));
    }
  });
  return [...members].sort((a,b) => a-b);
}

async function compareGroups() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  const membersA = getMembersForGroup(state, state.groupA);
  const membersB = getMembersForGroup(state, state.groupB);

  if (membersA.length < 2 || membersB.length < 2) {
    setStatus('Need at least 2 points in each group');
    return;
  }

  await runComparison(state, membersA, membersB, 'A vs B');
}

async function compareVsRest() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  const membersA = getMembersForGroup(state, state.groupA);
  if (membersA.length < 2) {
    setStatus('Need at least 2 points in Group A');
    return;
  }

  // "Rest" = all points not in group A
  const aSet = new Set(membersA);
  const allMembers = new Set();
  state.data.nodes.forEach(n => {
    (n.members || []).forEach(m => allMembers.add(m));
  });
  const membersB = [...allMembers].filter(m => !aSet.has(m)).sort((a,b) => a-b);

  if (membersB.length < 2) {
    setStatus('Rest group too small');
    return;
  }

  await runComparison(state, membersA, membersB, 'A vs Rest');
}

async function runComparison(state, membersA, membersB, title) {
  setStatus('Fetching column data for comparison...');
  const did = state.runInfo && state.runInfo['dataset-id'];
  if (!did) { setStatus('No dataset linked'); return; }

  try {
    // Fetch all columns at once (no cols param = all columns)
    const colData = await apiGet(`/api/dataset/${did}/columns`);

    let columns;
    if (Array.isArray(colData)) {
      columns = colData;
    } else if (colData.columns) {
      columns = colData.columns.map(c => c.values || c);
    } else {
      setStatus('Could not fetch column data'); return;
    }

    setStatus('Computing statistical tests...');

    // Run tests for each column
    const selectedTest = document.getElementById('stat-test-select').value;
    const results = [];
    let skippedCols = 0;
    for (let ci = 0; ci < columns.length; ci++) {
      const vals = columns[ci];
      const colName = state.colNames[ci] || `col_${ci}`;
      const valsA = membersA.map(m => vals[m]).filter(v => v !== null && v !== undefined);
      const valsB = membersB.map(m => vals[m]).filter(v => v !== null && v !== undefined);

      if (valsA.length < 2 || valsB.length < 2) continue;

      const allVals = valsA.concat(valsB);
      const numericCol = isNumericColumn(allVals);
      const categoricalCol = isCategoricalColumn(allVals);

      // Filter columns based on selected test
      if (selectedTest === 'hypergeometric' && !categoricalCol) { skippedCols++; continue; }
      if ((selectedTest === 'mann-whitney' || selectedTest === 'ks' || selectedTest === 'welch-t') && !numericCol) { skippedCols++; continue; }

      const numA = numericCol ? valsA.map(Number) : [];
      const numB = numericCol ? valsB.map(Number) : [];

      let result;
      if (selectedTest === 'auto') {
        // Auto: pick best test based on column type
        if (!numericCol || (categoricalCol && !numericCol)) {
          result = hypergeometricTest(valsA, valsB);
          result.test = 'hyper';
        } else {
          const tRes = welchTTest(numA, numB);
          const mwRes = mannWhitneyU(numA, numB);
          if (tRes.p <= mwRes.p) {
            result = tRes;
            result.test = 'welch-t';
          } else {
            result = mwRes;
            result.test = 'mann-whitney';
          }
        }
      } else if (selectedTest === 'hypergeometric') {
        result = hypergeometricTest(valsA, valsB);
        result.test = 'hyper';
      } else if (selectedTest === 'mann-whitney') {
        result = mannWhitneyU(numA, numB);
        result.test = 'mann-whitney';
      } else if (selectedTest === 'ks') {
        result = ksTest(numA, numB);
        result.test = 'ks';
      } else if (selectedTest === 'welch-t') {
        result = welchTTest(numA, numB);
        result.test = 'welch-t';
      }

      result.col = colName;
      result.colIdx = ci;
      results.push(result);
    }

    // FDR correction
    compResults = benjaminiHochberg(results);
    compSortKey = 'p';
    compSortAsc = true;

    renderCompResults(title, membersA.length, membersB.length);
    const skipMsg = skippedCols > 0 ? ` (${skippedCols} columns skipped — wrong type for ${selectedTest})` : '';
    setStatus(`Comparison done: ${compResults.length} columns tested${skipMsg}`);
  } catch(e) {
    setStatus('Error: ' + e.message);
    console.error(e);
  }
}

function renderCompResults(title, nA, nB) {
  // Store for re-render on filter change
  if (title) compTitle = title;
  if (nA) compNA = nA;
  if (nB) compNB = nB;

  const panel = document.getElementById('comparison-panel');
  panel.style.display = 'block';

  // Apply p-value filter
  const filterVal = parseFloat(document.getElementById('comp-pval-filter').value);
  const useQ = document.getElementById('comp-use-qval').checked;
  let filtered = compResults;
  if (!isNaN(filterVal) && filterVal > 0) {
    filtered = compResults.filter(r => (useQ ? r.q : r.p) <= filterVal);
  }

  document.getElementById('comparison-summary').textContent =
    `${compTitle}: ${compNA} vs ${compNB} points, ${filtered.length}/${compResults.length} columns` +
    (!isNaN(filterVal) && filterVal > 0 ? ` (${useQ ? 'q' : 'p'} \u2264 ${filterVal})` : '');

  const tbody = document.getElementById('comparison-body');
  tbody.innerHTML = '';

  for (const r of filtered) {
    const tr = document.createElement('tr');
    tr.className = 'clickable';
    if (r.q < 0.01) tr.classList.add('sig-high');
    else if (r.q < 0.05) tr.classList.add('sig-med');
    else tr.classList.add('sig-low');

    tr.innerHTML = `<td>${r.col}</td><td>${r.p.toExponential(2)}</td><td>${r.q.toExponential(2)}</td><td>${r.diff !== undefined ? r.diff.toFixed(3) : '--'}</td><td>${r.test}</td>`;

    // Click to recolor by this column
    tr.onclick = () => {
      document.getElementById('color-select').value = 'col:' + r.colIdx;
      recolor();
    };

    tbody.appendChild(tr);
  }
}

function onCompFilterChange() {
  if (compResults.length > 0) {
    renderCompResults();
  }
}

function exportCompCSV() {
  if (compResults.length === 0) {
    setStatus('No comparison results to export');
    return;
  }

  const rows = [['column', 'column_index', 'p_value', 'q_value', 'diff', 'test']];
  for (const r of compResults) {
    rows.push([
      '"' + (r.col || '').replace(/"/g, '""') + '"',
      r.colIdx,
      r.p,
      r.q,
      r.diff !== undefined ? r.diff : '',
      r.test,
    ]);
  }

  const csv = rows.map(r => r.join(',')).join('\n');
  const blob = new Blob([csv], { type: 'text/csv' });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = `comparison-${compTitle.replace(/\s+/g, '-')}.csv`;
  link.click();
  URL.revokeObjectURL(link.href);
  setStatus('Comparison CSV exported (' + compResults.length + ' rows)');
}

function sortCompTable(key) {
  if (compSortKey === key) {
    compSortAsc = !compSortAsc;
  } else {
    compSortKey = key;
    compSortAsc = true;
  }

  compResults.sort((a, b) => {
    let va, vb;
    if (key === 'col') { va = a.col; vb = b.col; }
    else if (key === 'p') { va = a.p; vb = b.p; }
    else if (key === 'q') { va = a.q; vb = b.q; }
    else if (key === 'diff') { va = a.diff || 0; vb = b.diff || 0; }
    else { va = a[key]; vb = b[key]; }

    if (typeof va === 'string') {
      return compSortAsc ? va.localeCompare(vb) : vb.localeCompare(va);
    }
    return compSortAsc ? va - vb : vb - va;
  });

  renderCompResults();
}

// ============================================================
// Label persistence
// ============================================================

async function saveLabel(name, color, nodeIndices, graphId) {
  try {
    await apiPost('/api/label', {
      graph_id: graphId,
      name: name,
      color: color,
      nodes: nodeIndices.join(','),
    });
  } catch(e) {
    console.error('Failed to save label:', e);
  }
}

async function loadLabels(graphId) {
  try {
    return await apiGet(`/api/labels/${graphId}`);
  } catch(e) {
    return [];
  }
}

// ============================================================
// Export
// ============================================================

function exportPNG() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state || !state.cy) return;

  const png = state.cy.png({ scale: 2, bg: '#1a1a2e' });
  const link = document.createElement('a');
  link.href = png;
  link.download = `mapper-graph-${activeTabId}.png`;
  link.click();
  setStatus('PNG exported');
}

function exportCSV() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  // Build CSV: row_id, node_id, group
  const rows = [['row_id', 'node_id', 'group']];
  const membersA = new Set(getMembersForGroup(state, state.groupA));
  const membersB = new Set(getMembersForGroup(state, state.groupB));

  state.data.nodes.forEach(n => {
    const group = state.groupA.has(n.id) ? 'A' : (state.groupB.has(n.id) ? 'B' : '');
    (n.members || []).forEach(m => {
      const memberGroup = membersA.has(m) ? 'A' : (membersB.has(m) ? 'B' : '');
      rows.push([m, n.id, memberGroup]);
    });
  });

  const csv = rows.map(r => r.join(',')).join('\n');
  const blob = new Blob([csv], { type: 'text/csv' });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = `mapper-graph-${activeTabId}.csv`;
  link.click();
  URL.revokeObjectURL(link.href);
  setStatus('CSV exported');
}

// ============================================================
// Upload dialog
// ============================================================

let pendingCsvText = null;

function showUploadDialog() {
  document.getElementById('upload-dialog').classList.add('visible');
  document.getElementById('csv-file').value = '';
  document.getElementById('dataset-name').value = '';
  document.getElementById('csv-preview').innerHTML = '';
  pendingCsvText = null;
}

function hideUploadDialog() {
  document.getElementById('upload-dialog').classList.remove('visible');
}

// Preview CSV when file is selected
document.getElementById('csv-file').addEventListener('change', function(e) {
  const file = e.target.files[0];
  if (!file) return;

  // Auto-fill name from filename (strip extension)
  const nameInput = document.getElementById('dataset-name');
  if (!nameInput.value) {
    nameInput.value = file.name.replace(/\.[^.]+$/, '');
  }

  const reader = new FileReader();
  reader.onload = function(ev) {
    pendingCsvText = ev.target.result;

    // Show preview of first 5 rows
    const lines = pendingCsvText.split('\n').filter(l => l.trim().length > 0);
    if (lines.length === 0) {
      document.getElementById('csv-preview').innerHTML = '<div class="preview-note">Empty file</div>';
      return;
    }

    const previewLines = lines.slice(0, 6); // header + 5 data rows
    let html = '<div class="preview-note">' + (lines.length - 1) + ' data rows</div>';
    html += '<table class="csv-preview-table"><thead><tr>';

    // Header
    const headers = previewLines[0].split(',').map(h => h.trim().replace(/^"|"$/g, ''));
    for (const h of headers) {
      html += '<th>' + escapeHtml(h) + '</th>';
    }
    html += '</tr></thead><tbody>';

    // Data rows (up to 5)
    for (let i = 1; i < previewLines.length; i++) {
      html += '<tr>';
      const fields = previewLines[i].split(',');
      for (let j = 0; j < headers.length; j++) {
        const val = fields[j] ? fields[j].trim() : '';
        html += '<td>' + escapeHtml(val) + '</td>';
      }
      html += '</tr>';
    }
    html += '</tbody></table>';
    if (lines.length > 6) {
      html += '<div class="preview-note">...and ' + (lines.length - 6) + ' more rows</div>';
    }

    document.getElementById('csv-preview').innerHTML = html;
  };
  reader.readAsText(file);
});

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

async function uploadDataset() {
  if (!pendingCsvText) {
    setStatus('No file selected');
    return;
  }

  const name = document.getElementById('dataset-name').value || 'uploaded';
  setStatus('Uploading dataset...');

  try {
    const resp = await fetch('/api/dataset?name=' + encodeURIComponent(name), {
      method: 'POST',
      headers: { 'Content-Type': 'text/csv' },
      body: pendingCsvText,
    });

    if (!resp.ok) {
      const err = await resp.json().catch(() => ({ error: resp.statusText }));
      throw new Error(err.error || 'Upload failed');
    }

    const result = await resp.json();
    setStatus('Dataset uploaded: ' + result.name + ' (' + result['n-rows'] + ' rows, ' + result['n-cols'] + ' cols)');
    hideUploadDialog();

    // Offer to run mapper on this dataset
    showRunDialog(result.id);
  } catch(e) {
    setStatus('Upload error: ' + e.message);
  }
}

// ============================================================
// Load server file dialog
// ============================================================

function showLoadFileDialog() {
  document.getElementById('load-file-path').value = '';
  document.getElementById('load-file-name').value = '';
  document.getElementById('load-file-status').textContent = '';
  document.getElementById('load-file-dialog').classList.add('visible');
}

function hideLoadFileDialog() {
  document.getElementById('load-file-dialog').classList.remove('visible');
}

async function loadServerFile() {
  const filepath = document.getElementById('load-file-path').value.trim();
  const name = document.getElementById('load-file-name').value.trim() || 'loaded';
  const statusEl = document.getElementById('load-file-status');

  if (!filepath) {
    statusEl.textContent = 'Please enter a file path';
    return;
  }

  statusEl.textContent = 'Loading... (large files may take a minute)';
  statusEl.style.color = '#888';

  try {
    const resp = await fetch('/api/load-file', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'path=' + encodeURIComponent(filepath) + '&name=' + encodeURIComponent(name),
    });

    if (!resp.ok) {
      const err = await resp.json().catch(() => ({ error: resp.statusText }));
      throw new Error(err.error || 'Load failed');
    }

    const result = await resp.json();
    statusEl.textContent = 'Loaded: ' + result.name + ' (' + result['n-rows'] + ' rows, ' + result['n-cols'] + ' cols)';
    statusEl.style.color = '#4a4';
    setStatus('Dataset loaded: ' + result.name + ' (' + result['n-rows'] + 'x' + result['n-cols'] + ')');

    // Close after short delay so user can see the result
    setTimeout(() => {
      hideLoadFileDialog();
      showRunDialog(result.id);
    }, 1500);
  } catch(e) {
    statusEl.textContent = 'Error: ' + e.message;
    statusEl.style.color = '#e55';
  }
}

// ============================================================
// Run dialog
// ============================================================

async function showRunDialog(preselectedDatasetId) {
  const dialog = document.getElementById('run-dialog');
  const datasetSelect = document.getElementById('run-dataset');

  // Fetch available datasets
  try {
    const datasets = await apiGet('/api/datasets');
    datasetSelect.innerHTML = '';

    if (datasets.length === 0) {
      datasetSelect.innerHTML = '<option value="">No datasets — upload one first</option>';
    } else {
      for (const ds of datasets) {
        const opt = document.createElement('option');
        opt.value = ds.id;
        opt.textContent = ds.name + ' (' + ds['n-rows'] + 'x' + ds['n-cols'] + ')';
        datasetSelect.appendChild(opt);
      }

      // Preselect if specified
      if (preselectedDatasetId) {
        datasetSelect.value = preselectedDatasetId;
      }
    }
  } catch(e) {
    datasetSelect.innerHTML = '<option value="">Error loading datasets</option>';
  }

  // Wire up dataset change listener and load columns for initial selection
  datasetSelect.onchange = onDatasetChange;
  onDatasetChange();

  // Reset lens 2 to (None) and column selection mode to "Top N by variance"
  document.getElementById('run-lens2').value = 'none';
  onLens2Change();
  const varRadio = document.querySelector('input[name="colsel-mode"][value="auto_variance"]');
  if (varRadio) varRadio.checked = true;
  onColSelModeChange();

  dialog.classList.add('visible');
}

function hideRunDialog() {
  document.getElementById('run-dialog').classList.remove('visible');
}

function onLens1Change() {
  const lens = document.getElementById('run-lens').value;
  document.getElementById('lens1-col-group').style.display =
    (lens === 'column') ? '' : 'none';
}

function onLens2Change() {
  const lens2 = document.getElementById('run-lens2').value;
  document.getElementById('lens2-col-group').style.display =
    (lens2 === 'column') ? '' : 'none';
}

// Column selection mode change
function onColSelModeChange() {
  const mode = document.querySelector('input[name="colsel-mode"]:checked').value;
  document.getElementById('colsel-manual-box').style.display = (mode === 'manual') ? '' : 'none';
  document.getElementById('colsel-topn-box').style.display = (mode === 'auto_variance') ? '' : 'none';
}

// Fetch column names when dataset changes and populate checkboxes
async function onDatasetChange() {
  const datasetId = document.getElementById('run-dataset').value;
  const container = document.getElementById('colsel-checkboxes');
  container.innerHTML = '';
  document.getElementById('colsel-count').textContent = '';
  if (!datasetId) return;

  try {
    const info = await apiGet('/api/dataset/' + datasetId);
    let colNames = info && info['col-names'];
    if (typeof colNames === 'string') colNames = JSON.parse(colNames);
    if (colNames && colNames.length > 0) {
      populateColSelCheckboxes(colNames);
      // Set max on column index inputs to prevent out-of-range
      const maxCol = colNames.length - 1;
      document.getElementById('run-colorcol').max = maxCol;
      document.getElementById('run-lens2-col').max = maxCol;
    }
  } catch(e) {
    container.innerHTML = '<span style="color:#666;font-size:11px">Could not load columns</span>';
  }
}

function populateColSelCheckboxes(names) {
  const container = document.getElementById('colsel-checkboxes');
  container.innerHTML = '';
  names.forEach((name, i) => {
    const lbl = document.createElement('label');
    lbl.innerHTML = '<input type="checkbox" value="' + i + '" checked onchange="updateColSelCount()"> ' + escapeHtml(name);
    container.appendChild(lbl);
  });
  updateColSelCount();
}

function colSelAll() {
  document.querySelectorAll('#colsel-checkboxes input[type="checkbox"]').forEach(cb => cb.checked = true);
  updateColSelCount();
}

function colSelNone() {
  document.querySelectorAll('#colsel-checkboxes input[type="checkbox"]').forEach(cb => cb.checked = false);
  updateColSelCount();
}

function updateColSelCount() {
  const all = document.querySelectorAll('#colsel-checkboxes input[type="checkbox"]');
  const checked = document.querySelectorAll('#colsel-checkboxes input[type="checkbox"]:checked');
  document.getElementById('colsel-count').textContent = checked.length + '/' + all.length + ' selected';
}

async function startRun() {
  const datasetId = document.getElementById('run-dataset').value;
  if (!datasetId) {
    setStatus('No dataset selected');
    return;
  }

  // Build column_selection JSON
  const colSelMode = document.querySelector('input[name="colsel-mode"]:checked').value;
  const colorCol = parseInt(document.getElementById('run-colorcol').value) || 0;
  const lensType2 = document.getElementById('run-lens2').value;
  const lens2Col = parseInt(document.getElementById('run-lens2-col').value) || 0;

  let colSelObj;
  if (colSelMode === 'manual') {
    const checkedCols = [];
    document.querySelectorAll('#colsel-checkboxes input[type="checkbox"]:checked').forEach(cb => {
      checkedCols.push(parseInt(cb.value));
    });
    colSelObj = { mode: 'manual', cols: checkedCols, color_col: colorCol };
  } else if (colSelMode === 'auto_variance') {
    const topN = parseInt(document.getElementById('colsel-topn').value) || 10;
    colSelObj = { mode: 'auto_variance', top_n: topN, cols: [], color_col: colorCol };
  } else {
    colSelObj = { mode: 'all', cols: [], color_col: colorCol };
  }

  // Add second lens info if selected
  if (lensType2 && lensType2 !== 'none') {
    colSelObj.lens_type_2 = lensType2;
    if (lensType2 === 'column') {
      colSelObj.lens2_col = lens2Col;
    }
  }

  const columnSelection = JSON.stringify(colSelObj);

  const params = {
    dataset_id: datasetId,
    name: document.getElementById('run-name').value || 'mapper run',
    n_cubes: document.getElementById('run-ncubes').value,
    overlap: document.getElementById('run-overlap').value,
    metric: document.getElementById('run-metric').value,
    clusterer: document.getElementById('run-clusterer').value,
    eps: document.getElementById('run-eps').value,
    min_pts: document.getElementById('run-minpts').value,
    min_intersection: document.getElementById('run-minintersect').value,
    lens_type: document.getElementById('run-lens').value,
    column_selection: columnSelection,
  };

  setStatus('Starting mapper run...');
  hideRunDialog();

  try {
    const result = await apiPost('/api/run', params);
    const runId = result['run-id'];
    setStatus('Mapper run #' + runId + ' started');
    startRunPolling(runId);
  } catch(e) {
    setStatus('Error starting run: ' + e.message);
  }
}

// ============================================================
// Run status polling
// ============================================================

let activeRunPolls = new Map(); // runId -> intervalId

function startRunPolling(runId) {
  const statusEl = document.getElementById('run-status');
  statusEl.textContent = 'Running mapper (#' + runId + ')...';
  statusEl.classList.add('active');

  const pollId = setInterval(async () => {
    try {
      const info = await apiGet('/api/run/' + runId);
      if (info.status === 'completed') {
        clearInterval(pollId);
        activeRunPolls.delete(runId);
        statusEl.textContent = '';
        statusEl.classList.remove('active');
        setStatus('Run #' + runId + ' completed');
        console.log('Run', runId, 'completed, polling for new graphs...');
        // pollForGraphs will auto-open the new graph
        await pollForGraphs();
      } else if (info.status === 'failed') {
        clearInterval(pollId);
        activeRunPolls.delete(runId);
        statusEl.textContent = '';
        statusEl.classList.remove('active');
        setStatus('Run #' + runId + ' failed: ' + (info['error-message'] || 'unknown error'));
      }
      // else still running/pending — keep polling
    } catch(e) {
      // Ignore poll errors
    }
  }, 2000);

  activeRunPolls.set(runId, pollId);
}

// ============================================================
// Help panel toggle
// ============================================================

function toggleHelp() {
  const content = document.getElementById('help-content');
  const arrow = document.getElementById('help-arrow');
  if (content.style.display === 'none') {
    content.style.display = 'block';
    arrow.classList.add('open');
  } else {
    content.style.display = 'none';
    arrow.classList.remove('open');
  }
}

// ============================================================
// Initialization
// ============================================================

document.getElementById('tab-add').onclick = showGraphPicker;
document.getElementById('graph-picker').onclick = function(e) {
  if (e.target === this) hideGraphPicker();
};
document.getElementById('upload-dialog').onclick = function(e) {
  if (e.target === this) hideUploadDialog();
};
document.getElementById('run-dialog').onclick = function(e) {
  if (e.target === this) hideRunDialog();
};

// Initial poll
pollForGraphs();

// Poll every 5 seconds for new graphs
pollInterval = setInterval(pollForGraphs, 5000);

setStatus('Ready');
