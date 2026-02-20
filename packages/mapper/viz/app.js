// Mapper Visualization Tool - app.js
// Cytoscape.js-based graph visualization with multi-tab support

'use strict';

// ============================================================
// State
// ============================================================

const openTabs = new Map();   // graphId -> { cy, data, runInfo, groupA, groupB }
let activeTabId = null;
let activeGroup = 'A';
let pollInterval = null;

// ============================================================
// API client
// ============================================================

async function apiGet(path) {
  const resp = await fetch(path);
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
  // Dark purple -> teal -> yellow (viridis-inspired)
  t = Math.max(0, Math.min(1, t));
  let r, g, b;
  if (t < 0.25) {
    const s = t / 0.25;
    r = Math.round(68 + s * (49 - 68));
    g = Math.round(1 + s * (104 - 1));
    b = Math.round(84 + s * (142 - 84));
  } else if (t < 0.5) {
    const s = (t - 0.25) / 0.25;
    r = Math.round(49 + s * (33 - 49));
    g = Math.round(104 + s * (144 - 104));
    b = Math.round(142 + s * (141 - 142));
  } else if (t < 0.75) {
    const s = (t - 0.5) / 0.25;
    r = Math.round(33 + s * (93 - 33));
    g = Math.round(144 + s * (201 - 144));
    b = Math.round(141 + s * (99 - 141));
  } else {
    const s = (t - 0.75) / 0.25;
    r = Math.round(93 + s * (253 - 93));
    g = Math.round(201 + s * (231 - 201));
    b = Math.round(99 + s * (37 - 99));
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
      layout: { name: 'fcose', animate: true, animationDuration: 800 },
      minZoom: 0.1,
      maxZoom: 10,
      boxSelectionEnabled: true,
    });

    // Store state
    const state = {
      cy, data: graphData, runInfo, containerEl: container,
      name, minColor, maxColor,
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
        'border-width': 3,
        'border-color': '#ffff00',
      }
    },
    {
      selector: 'node.groupA',
      style: {
        'border-width': 3,
        'border-color': '#ff4444',
      }
    },
    {
      selector: 'node.groupB',
      style: {
        'border-width': 3,
        'border-color': '#4488ff',
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

  // Box selection
  cy.on('boxend', function() {
    const selected = cy.nodes(':selected');
    const state = openTabs.get(graphId);
    if (!state) return;

    const grp = activeGroup === 'A' ? state.groupA : state.groupB;
    const other = activeGroup === 'A' ? state.groupB : state.groupA;

    selected.forEach(node => {
      const idx = node.data('nodeIdx');
      grp.add(idx);
      other.delete(idx);
    });

    cy.nodes().unselect();
    updateGroupClasses(cy, state);
    updateSidePanel();
  });
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
// Layout
// ============================================================

function runLayout() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  const layoutName = document.getElementById('layout-select').value;
  state.cy.layout({
    name: layoutName,
    animate: true,
    animationDuration: 800,
    randomize: layoutName === 'fcose',
  }).run();
}

// ============================================================
// Recolor
// ============================================================

function recolor() {
  if (!activeTabId) return;
  const state = openTabs.get(activeTabId);
  if (!state) return;

  const field = document.getElementById('color-select').value;
  state.colorField = field;

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

// ============================================================
// Toolbar
// ============================================================

function updateToolbar() {
  // Reset color select
  document.getElementById('color-select').value = 'default';
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
    // Auto-open any graph that's active and not yet open
    for (const g of graphs) {
      if (g.active === 1 && !openTabs.has(g.id)) {
        await openGraph(g.id);
      }
    }
  } catch(e) {
    // Silent failure on poll
  }
}

// ============================================================
// Initialization
// ============================================================

document.getElementById('tab-add').onclick = showGraphPicker;
document.getElementById('graph-picker').onclick = function(e) {
  if (e.target === this) hideGraphPicker();
};

// Initial poll
pollForGraphs();

// Poll every 5 seconds for new graphs
pollInterval = setInterval(pollForGraphs, 5000);

setStatus('Ready');
