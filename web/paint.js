(() => {
  const canvas = document.getElementById('canvas');
  const ctx = canvas.getContext('2d');
  const W = 480, H = 480;
  canvas.width = W;
  canvas.height = H;

  const PAPER = '#FAFAF8';

  // ---- 场景（浏览器本地镜像）----
  const strokes = [];     // 完整笔画，按绘制顺序
  let history = [];       // 已执行操作栈 {kind:'add',s} | {kind:'clear',snapshot}
  let redoOps = [];       // 被撤销的操作（redo 前向重放）
  let viewport = { scale: 1, x: 0, y: 0 };

  let tool = 'draw';      // draw | pan
  let brush = 'pen';      // pen | marker | highlighter | eraser
  let color = '#37352F';
  let width = 3;

  // ---- WebSocket ----
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws`);
  const statusEl = document.getElementById('status');
  const statusText = statusEl.querySelector('.text');
  ws.onopen = () => { statusEl.classList.add('online'); statusText.textContent = '已连接'; };
  ws.onclose = () => { statusEl.classList.remove('online'); statusText.textContent = '已断开'; };
  const send = (o) => { if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o)); };

  // ---- 笔刷参数：透明度 + 宽度系数（最终宽度 = 滑块值 × 系数，双端一致）----
  const ALPHA = { pen: 255, marker: 170, highlighter: 110, eraser: 255 };
  const WIDTH_FACTOR = { pen: 1, marker: 1, highlighter: 2, eraser: 2.5 };

  // 笔画 id：同一次落笔的所有增量段共享（板端据此合并 + 按笔撤销）
  let strokeSeq = 0;
  const nextId = () => ++strokeSeq;

  // ---- 坐标变换 ----
  const toWorld = (sx, sy) => ({ x: sx / viewport.scale + viewport.x, y: sy / viewport.scale + viewport.y });
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  // ---- 渲染 ----
  function drawStroke(s) {
    if (!s.points || s.points.length < 2) return;
    ctx.save();
    ctx.globalAlpha = (s.alpha ?? 255) / 255;
    ctx.strokeStyle = s.brush === 'eraser' ? PAPER : s.color;
    ctx.lineWidth = s.width;
    ctx.lineCap = s.brush === 'highlighter' ? 'butt' : 'round';
    ctx.lineJoin = 'round';
    ctx.beginPath();
    ctx.moveTo(s.points[0], s.points[1]);
    for (let i = 2; i < s.points.length; i += 2) ctx.lineTo(s.points[i], s.points[i + 1]);
    ctx.stroke();
    ctx.restore();
  }
  function redraw() {
    ctx.clearRect(0, 0, W, H);
    ctx.save();
    ctx.setTransform(viewport.scale, 0, 0, viewport.scale,
                     -viewport.x * viewport.scale, -viewport.y * viewport.scale);
    for (const s of strokes) drawStroke(s);
    if (current) drawStroke(current);
    ctx.restore();
  }
  const sendViewport = () => send({ type: 'viewport', scale: viewport.scale, x: viewport.x, y: viewport.y });

  // ---- 本地撤销/重做（与板端 history 语义一致）----
  function copyStrokes() { return strokes.map((s) => ({ ...s, points: s.points.slice() })); }
  function removeByLastId(id) {
    for (let i = strokes.length - 1; i >= 0; --i) {
      if (strokes[i].id === id) { strokes.splice(i, 1); return; }
    }
  }
  function undo() {
    if (drawing || !history.length) return;
    const op = history.pop();
    if (op.kind === 'add') {
      removeByLastId(op.s.id);
    } else {
      strokes.splice(0, strokes.length, ...op.snapshot.map((s) => ({ ...s, points: s.points.slice() })));
    }
    redoOps.push(op);
    send({ type: 'undo' });
    redraw();
  }
  function redo() {
    if (drawing || !redoOps.length) return;
    const op = redoOps.pop();
    if (op.kind === 'add') {
      strokes.push(op.s);
    } else {
      strokes.length = 0;
    }
    history.push(op);
    send({ type: 'redo' });
    redraw();
  }

  // ---- 绘制 / 平移状态 ----
  let current = null;
  let drawing = false;
  let panning = false;
  let lastX = 0, lastY = 0;

  // 实时增量上屏：移动时按节流发「上一已发点 → 当前点」线段，带 id 由板端合并
  let lastSent = null;
  let sentAny = false;
  let lastSendAt = 0;
  const SEND_INTERVAL_MS = 33;

  function msgBase() {
    // 一律取落笔瞬间捕获的属性（current），避免绘画过程中全局态变化串笔
    return { type: 'draw', id: current.id, brush: current.brush,
             alpha: current.alpha, color: current.color, width: current.width };
  }
  function effWidth() { return Math.max(1, Math.round(width * WIDTH_FACTOR[brush])); }
  function sendSegmentTo(wx, wy) {
    if (!lastSent) return;
    const o = msgBase();
    o.points = [lastSent.x, lastSent.y, wx, wy];
    send(o);
    lastSent = { x: wx, y: wy };
    sentAny = true;
  }
  function pointerDown(wx, wy) {
    drawing = true;
    current = {
      id: nextId(), brush, alpha: ALPHA[brush],
      color: brush === 'eraser' ? PAPER : color,
      width: effWidth(),
      points: [wx, wy],
    };
    lastSent = { x: wx, y: wy };
    sentAny = false;
    lastSendAt = 0;
  }
  function pointerMoveTo(wx, wy) {
    current.points.push(wx, wy);
    redraw();
    if (lastSent && (wx !== lastSent.x || wy !== lastSent.y)) {
      const now = performance.now();
      if (!sentAny || now - lastSendAt >= SEND_INTERVAL_MS) {
        sendSegmentTo(wx, wy);
        lastSendAt = now;
      }
    }
  }
  function pointerUp() {
    if (!drawing || !current) return;
    drawing = false;
    if (current.points.length >= 2) {
      if (!sentAny) {
        // 原地单击：发单点（同 id），板端落一个点
        const o = msgBase();
        o.points = current.points.slice();
        send(o);
      } else if (lastSent) {
        const n = current.points.length;
        const lx = current.points[n - 2];
        const ly = current.points[n - 1];
        if (lx !== lastSent.x || ly !== lastSent.y) {
          sendSegmentTo(lx, ly);   // 补发收尾段
        }
      }
      strokes.push(current);
      history.push({ kind: 'add', s: current });
      redoOps.length = 0;
    }
    current = null;
    lastSent = null;
    sentAny = false;
    redraw();
  }

  function pointerPos(e) {
    const r = canvas.getBoundingClientRect();
    return { x: (e.clientX - r.left) * (W / r.width), y: (e.clientY - r.top) * (H / r.height) };
  }

  // ---- 指针事件 ----
  canvas.addEventListener('pointerdown', (e) => {
    canvas.setPointerCapture(e.pointerId);
    const p = pointerPos(e);
    if (tool === 'pan' || e.button === 1) {
      panning = true;
      lastX = p.x; lastY = p.y;
      return;
    }
    const w = toWorld(p.x, p.y);
    pointerDown(Math.round(w.x), Math.round(w.y));
  });

  canvas.addEventListener('pointermove', (e) => {
    const p = pointerPos(e);
    if (panning) {
      viewport.x -= (p.x - lastX) / viewport.scale;
      viewport.y -= (p.y - lastY) / viewport.scale;
      lastX = p.x; lastY = p.y;
      sendViewport();
      redraw();
      return;
    }
    if (drawing && current) {
      const w = toWorld(p.x, p.y);
      pointerMoveTo(Math.round(w.x), Math.round(w.y));
    }
  });

  function endPointer(e) {
    if (panning) { panning = false; return; }
    if (drawing && current && e) {
      const p = pointerPos(e);
      const w = toWorld(p.x, p.y);
      pointerMoveTo(Math.round(w.x), Math.round(w.y));
    }
    pointerUp();
  }
  canvas.addEventListener('pointerup', endPointer);
  canvas.addEventListener('pointercancel', endPointer);

  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const p = pointerPos(e);
    const w = toWorld(p.x, p.y);
    const factor = e.deltaY < 0 ? 1.1 : 0.9;
    viewport.scale = clamp(viewport.scale * factor, 0.1, 10);
    viewport.x = w.x - p.x / viewport.scale;
    viewport.y = w.y - p.y / viewport.scale;
    sendViewport();
    redraw();
  }, { passive: false });

  // ---- 键盘：撤销/重做 ----
  document.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = e.key.toLowerCase();
    if (k === 'z' && e.shiftKey) { e.preventDefault(); redo(); }
    else if (k === 'z') { e.preventDefault(); undo(); }
    else if (k === 'y') { e.preventDefault(); redo(); }
  });

  // ---- 工具栏 ----
  document.querySelectorAll('.tool').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tool').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      tool = btn.dataset.tool;
      canvas.style.cursor = tool === 'pan' ? 'grab' : 'crosshair';
    });
  });
  document.querySelectorAll('.brush').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.brush').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      brush = btn.dataset.brush;
    });
  });
  document.querySelectorAll('.swatch').forEach((s) => {
    s.addEventListener('click', () => {
      document.querySelectorAll('.swatch').forEach((x) => x.classList.remove('active'));
      s.classList.add('active');
      color = s.dataset.color;
    });
  });
  const picker = document.getElementById('color-picker');
  picker.addEventListener('input', () => {
    document.querySelectorAll('.swatch').forEach((x) => x.classList.remove('active'));
    color = picker.value;
  });
  const widthInput = document.getElementById('width');
  const widthVal = document.getElementById('width-val');
  widthInput.addEventListener('input', () => { width = +widthInput.value; widthVal.textContent = width; });

  document.getElementById('undo').addEventListener('click', undo);
  document.getElementById('redo').addEventListener('click', redo);
  document.getElementById('clear').addEventListener('click', () => {
    if (!strokes.length) return;
    history.push({ kind: 'clear', snapshot: copyStrokes() });
    redoOps.length = 0;
    strokes.length = 0;
    current = null;
    redraw();
    send({ type: 'clear' });
  });

  redraw();
})();
