(() => {
  const canvas = document.getElementById('canvas');
  const ctx = canvas.getContext('2d');
  const W = 480, H = 480;
  canvas.width = W;
  canvas.height = H;

  // 世界状态（与板端一致：screen = (world - offset) * scale）
  const strokes = [];
  let viewport = { scale: 1, x: 0, y: 0 };
  let tool = 'draw';
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

  // ---- 坐标变换 ----
  const toWorld = (sx, sy) => ({ x: sx / viewport.scale + viewport.x, y: sy / viewport.scale + viewport.y });
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  // ---- 渲染 ----
  function drawStroke(s) {
    if (!s.points || s.points.length < 2) return;
    ctx.strokeStyle = s.color;
    ctx.lineWidth = s.width;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.beginPath();
    ctx.moveTo(s.points[0], s.points[1]);
    for (let i = 2; i < s.points.length; i += 2) ctx.lineTo(s.points[i], s.points[i + 1]);
    ctx.stroke();
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

  // ---- 绘制 / 平移状态 ----
  let current = null;
  let drawing = false;
  let panning = false;
  let lastX = 0, lastY = 0;

  // 实时增量上屏：画笔移动时按节流把"上一已发点 → 当前点"的小线段发给板端，
  // 板端逐段渲染，实现"画到哪同步到哪"，无需等整笔结束（v0.1.1）。
  let lastSent = null;      // 上次已发送到板端的（世界坐标）点
  let sentAny = false;      // 本笔是否已发过至少一段
  let lastSendAt = 0;       // 上次发送时刻（performance.now）
  const SEND_INTERVAL_MS = 33; // ≈30 段/秒：视觉无感，同时压低板端重绘频率

  function sendSegmentTo(wx, wy) {
    if (!lastSent) return;
    send({ type: 'draw', color: current.color, width: current.width,
           points: [lastSent.x, lastSent.y, wx, wy] });
    lastSent = { x: wx, y: wy };
    sentAny = true;
  }
  function pointerDown(wx, wy) {
    drawing = true;
    current = { color, width, points: [wx, wy] };
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
        // 原地按下即抬起（单击）：发单点，板端落一个圆点
        send({ type: 'draw', color: current.color, width: current.width, points: current.points });
      } else if (lastSent) {
        // 补发节流间隙内未上屏的最后一段，保证笔迹收尾无缺口
        const n = current.points.length;
        const lx = current.points[n - 2];
        const ly = current.points[n - 1];
        if (lx !== lastSent.x || ly !== lastSent.y) {
          sendSegmentTo(lx, ly);
        }
      }
      strokes.push(current);
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
      pointerMoveTo(Math.round(w.x), Math.round(w.y)); // 收掉最后一帧（含本地预览）
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

  // ---- 工具栏 ----
  document.querySelectorAll('.tool').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tool').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      tool = btn.dataset.tool;
      canvas.style.cursor = tool === 'pan' ? 'grab' : 'crosshair';
    });
  });
  document.querySelectorAll('.swatch').forEach((s) => {
    s.addEventListener('click', () => {
      document.querySelectorAll('.swatch').forEach((x) => x.classList.remove('active'));
      s.classList.add('active');
      color = s.dataset.color;
    });
  });
  const widthInput = document.getElementById('width');
  const widthVal = document.getElementById('width-val');
  widthInput.addEventListener('input', () => { width = +widthInput.value; widthVal.textContent = width; });
  document.getElementById('clear').addEventListener('click', () => {
    strokes.length = 0;
    current = null;
    redraw();
    send({ type: 'clear' });
  });

  redraw();
})();
