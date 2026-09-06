(() => {
  const canvas = document.getElementById('canvas');
  const ctx = canvas.getContext('2d');
  const W = 480, H = 480;
  canvas.width = W;
  canvas.height = H;

  const PAPER = '#FAFAF8';
  const IMG_MAX_DIM = 320;   // 粘贴位图上传上限（板端分辨率有限，超限等比缩小）

  // ---- 场景（浏览器本地镜像）：统一对象列表，笔画与图片按添加顺序交错 ----
  const scene = [];          // {kind:'stroke', s} | {kind:'img', im}
  const bitmaps = new Map(); // imgId -> HTMLCanvasElement（位图源，drawImage 用）
  let history = [];          // {kind:'add-stroke',id}|{kind:'add-img',id}|{kind:'rm-img',im}|{kind:'clear',snapshot}
  let redoOps = [];
  let viewport = { scale: 1, x: 0, y: 0 };

  let tool = 'draw';         // draw | pan
  let brush = 'pen';         // pen | marker | highlighter | eraser
  let color = '#37352F';
  let width = 3;

  let itemSeq = 1000;        // 场景对象 id（笔画/图片共享递增）
  let imgSeq = 1;            // 位图 id（img-data 缓存键）
  const nextItemId = () => ++itemSeq;
  const nextImgId = () => ++imgSeq;

  // ---- WebSocket ----
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws`);
  const statusEl = document.getElementById('status');
  const statusText = statusEl.querySelector('.text');
  ws.onopen = () => { statusEl.classList.add('online'); statusText.textContent = '已连接'; };
  ws.onclose = () => { statusEl.classList.remove('online'); statusText.textContent = '已断开'; };
  const send = (o) => { if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o)); };

  // ---- 笔刷参数 ----
  const ALPHA = { pen: 255, marker: 170, highlighter: 110, eraser: 255 };
  const WIDTH_FACTOR = { pen: 1, marker: 1, highlighter: 2, eraser: 2.5 };

  // ---- 坐标变换 ----
  const toWorld = (sx, sy) => ({ x: sx / viewport.scale + viewport.x, y: sy / viewport.scale + viewport.y });
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  // ---- 渲染 ----
  function drawStrokeItem(s) {
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
  function drawImageItem(im) {
    const bmp = bitmaps.get(im.img);
    if (!bmp) return;
    ctx.save();
    ctx.translate(im.cx, im.cy);
    ctx.rotate(im.rot);
    ctx.drawImage(bmp, -im.w / 2, -im.h / 2, im.w, im.h);
    ctx.restore();
  }
  function drawItem(it) {
    if (it.kind === 'stroke') drawStrokeItem(it.s);
    else drawImageItem(it.im);
  }
  function redraw() {
    ctx.clearRect(0, 0, W, H);
    ctx.save();
    ctx.setTransform(viewport.scale, 0, 0, viewport.scale,
                     -viewport.x * viewport.scale, -viewport.y * viewport.scale);
    for (const it of scene) drawItem(it);
    if (current) drawStrokeItem(current);
    drawSelectionOverlay();
    ctx.restore();
  }
  function drawSelectionOverlay() {
    if (!selectedImg) return;
    const im = selectedImg;
    const cos = Math.cos(im.rot), sin = Math.sin(im.rot);
    const hw = im.w / 2, hh = im.h / 2;
    const corners = [[-hw, -hh], [hw, -hh], [hw, hh], [-hw, hh]].map(([lx, ly]) => ({
      x: im.cx + lx * cos - ly * sin,
      y: im.cy + lx * sin + ly * cos,
    }));
    ctx.save();
    ctx.strokeStyle = '#5B6AF0';
    ctx.lineWidth = 1.2 / viewport.scale;
    ctx.setLineDash([4 / viewport.scale, 3 / viewport.scale]);
    ctx.beginPath();
    corners.forEach((p, i) => (i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
    ctx.closePath();
    ctx.stroke();
    corners.forEach((p) => {
      ctx.setLineDash([]);
      ctx.fillStyle = '#FFFFFF';
      ctx.strokeStyle = '#5B6AF0';
      ctx.lineWidth = 1.2 / viewport.scale;
      const r = 3.5 / viewport.scale;
      ctx.beginPath();
      ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
    });
    ctx.restore();
  }
  const sendViewport = () => send({ type: 'viewport', scale: viewport.scale, x: viewport.x, y: viewport.y });

  // ---- 网络节流（视口 / 图片几何 33ms coalesce，避免洪峰）----
  let vpDirty = false, vpLastSent = 0;
  let geoDirty = false;
  const NET_INTERVAL_MS = 33;
  function sendImgGeo(im) {
    send({ type: 'img-update', id: im.id, img: im.img,
           cx: Math.round(im.cx * 100) / 100, cy: Math.round(im.cy * 100) / 100,
           w: Math.round(im.w * 100) / 100, h: Math.round(im.h * 100) / 100,
           rot: Math.round(im.rot * 1000) / 1000 });
  }
  function flushPending() {
    if (vpDirty && ws.readyState === WebSocket.OPEN) {
      vpDirty = false;
      vpLastSent = performance.now();
      sendViewport();
    }
    if (geoDirty && selectedImg && ws.readyState === WebSocket.OPEN) {
      geoDirty = false;
      sendImgGeo(selectedImg);
    }
  }
  function requestVpSync() {
    vpDirty = true;
    if (performance.now() - vpLastSent >= NET_INTERVAL_MS) flushPending();
  }
  setInterval(flushPending, NET_INTERVAL_MS);

  // ---- 本地撤销/重做（与板端 history 语义一致）----
  function snapshotScene() {
    return scene.map((it) => it.kind === 'stroke'
      ? { kind: 'stroke', s: { ...it.s, points: it.s.points.slice() } }
      : { kind: 'img', im: { ...it.im } });
  }
  function removeSceneById(id) {
    for (let i = scene.length - 1; i >= 0; --i) {
      if ((scene[i].kind === 'stroke' && scene[i].s.id === id) ||
          (scene[i].kind === 'img' && scene[i].im.id === id)) {
        scene.splice(i, 1);
        return;
      }
    }
  }
  function undo() {
    if (drawing || dragImgMode || !history.length) return;
    const op = history.pop();
    if (op.kind === 'add-stroke' || op.kind === 'add-img') {
      removeSceneById(op.id);
    } else if (op.kind === 'rm-img') {
      scene.push({ kind: 'img', im: op.im });
    } else {   // clear
      scene.splice(0, scene.length, ...snapshotScene());
    }
    redoOps.push(op);
    send({ type: 'undo' });
    selectedImg = null;
    redraw();
  }
  function redo() {
    if (drawing || dragImgMode || !redoOps.length) return;
    const op = redoOps.pop();
    if (op.kind === 'add-stroke') scene.push({ kind: 'stroke', s: op.s });
    else if (op.kind === 'add-img') scene.push({ kind: 'img', im: op.im });
    else if (op.kind === 'rm-img') removeSceneById(op.im.id);
    else scene.length = 0;
    history.push(op);
    send({ type: 'redo' });
    selectedImg = null;
    redraw();
  }
  function pushOp(op) { history.push(op); redoOps.length = 0; }

  // ---- 绘制状态（笔画增量实时上屏）----
  let current = null, drawing = false, panning = false;
  let lastX = 0, lastY = 0;
  let lastSent = null, sentAny = false, lastSendAt = 0;
  function msgBase() {
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
  function pointerDownStroke(wx, wy) {
    drawing = true;
    current = {
      id: nextItemId(), brush, alpha: ALPHA[brush],
      color: brush === 'eraser' ? PAPER : color,
      width: effWidth(),
      points: [wx, wy],
    };
    lastSent = { x: wx, y: wy };
    sentAny = false;
    lastSendAt = 0;
  }
  function pointerMoveStroke(wx, wy) {
    current.points.push(wx, wy);
    redraw();
    if (lastSent && (wx !== lastSent.x || wy !== lastSent.y)) {
      const now = performance.now();
      if (!sentAny || now - lastSendAt >= 33) {
        sendSegmentTo(wx, wy);
        lastSendAt = now;
      }
    }
  }
  function pointerUpStroke() {
    if (!drawing || !current) return;
    drawing = false;
    if (current.points.length >= 2) {
      if (!sentAny) {
        const o = msgBase();
        o.points = current.points.slice();
        send(o);
      } else if (lastSent) {
        const n = current.points.length;
        const lx = current.points[n - 2], ly = current.points[n - 1];
        if (lx !== lastSent.x || ly !== lastSent.y) sendSegmentTo(lx, ly);
      }
      scene.push({ kind: 'stroke', s: current });
      pushOp({ kind: 'add-stroke', id: current.id, s: current });
    }
    current = null; lastSent = null; sentAny = false;
    redraw();
  }

  // ---- 图片：命中/选中/拖拽/删除/变换 ----
  let selectedImg = null;
  let dragImgMode = false;
  let dragGrab = null;         // 拾取点相对图片中心的偏移（世界坐标）
  let imgDragMoved = false;

  function pickImage(wx, wy) {
    for (let i = scene.length - 1; i >= 0; --i) {
      const it = scene[i];
      if (it.kind !== 'img') continue;
      const im = it.im;
      const dx = wx - im.cx, dy = wy - im.cy;
      const cos = Math.cos(-im.rot), sin = Math.sin(-im.rot);
      const lx = dx * cos - dy * sin, ly = dx * sin + dy * cos;
      if (Math.abs(lx) <= im.w / 2 && Math.abs(ly) <= im.h / 2) {
        return im;
      }
    }
    return null;
  }

  // 粘贴图片：等比缩到 IMG_MAX_DIM，透明合成到纸面 → RGB565 LE → 上传 + 本地对象
  function importImage(imgEl) {
    const rawW = imgEl.naturalWidth || imgEl.width;
    const rawH = imgEl.naturalHeight || imgEl.height;
    if (!rawW || !rawH) return;
    const k = Math.min(1, IMG_MAX_DIM / Math.max(rawW, rawH));
    const cw = Math.max(1, Math.round(rawW * k));
    const ch = Math.max(1, Math.round(rawH * k));
    const tmp = document.createElement('canvas');
    tmp.width = cw; tmp.height = ch;
    const tctx = tmp.getContext('2d', { willReadFrequently: true });
    tctx.fillStyle = PAPER;
    tctx.fillRect(0, 0, cw, ch);
    tctx.drawImage(imgEl, 0, 0, cw, ch);
    const data = tctx.getImageData(0, 0, cw, ch).data;

    const bytes = new Uint8Array(cw * ch * 2);
    const paper = [250, 250, 248];
    for (let i = 0; i < cw * ch; ++i) {
      const a = data[i * 4 + 3] / 255;
      const r = Math.round(data[i * 4] * a + paper[0] * (1 - a));
      const g = Math.round(data[i * 4 + 1] * a + paper[1] * (1 - a));
      const b = Math.round(data[i * 4 + 2] * a + paper[2] * (1 - a));
      const c16 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      bytes[i * 2] = c16 & 0xFF;
      bytes[i * 2 + 1] = c16 >> 8;
    }
    let bin = '';
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    const b64 = btoa(bin);

    const imgId = nextImgId();
    send({ type: 'img-data', img: imgId, w: cw, h: ch, data: b64 });

    const src = document.createElement('canvas');
    src.width = cw; src.height = ch;
    src.getContext('2d').drawImage(imgEl, 0, 0, cw, ch);
    bitmaps.set(imgId, src);

    const im = {
      id: nextItemId(), img: imgId,
      cx: viewport.x + (W / 2) / viewport.scale,
      cy: viewport.y + (H / 2) / viewport.scale,
      w: cw, h: ch, rot: 0,
    };
    scene.push({ kind: 'img', im });
    pushOp({ kind: 'add-img', id: im.id, im });
    selectedImg = im;
    send({ type: 'img', id: im.id, img: im.img, cx: im.cx, cy: im.cy, w: im.w, h: im.h, rot: im.rot });
    redraw();
  }

  document.addEventListener('paste', (e) => {
    const items = (e.clipboardData || {}).items;
    if (!items) return;
    for (const it of items) {
      if (it.type && it.type.startsWith('image/')) {
        e.preventDefault();
        const file = it.getAsFile();
        if (!file) return;
        const url = URL.createObjectURL(file);
        const imgEl = new Image();
        imgEl.onload = () => { importImage(imgEl); URL.revokeObjectURL(url); };
        imgEl.src = url;
        return;
      }
    }
  });

  function deleteSelectedImg() {
    if (!selectedImg) return;
    const im = selectedImg;
    selectedImg = null;
    removeSceneById(im.id);
    pushOp({ kind: 'rm-img', im });
    send({ type: 'img-remove', id: im.id });
    redraw();
  }
  function rotateSelectedImg(delta) {
    if (!selectedImg) return;
    selectedImg.rot += delta;
    geoDirty = true;
    redraw();
  }
  function zoomSelectedImg(factor) {
    if (!selectedImg) return;
    const im = selectedImg;
    im.w = Math.max(8, Math.round(im.w * factor));
    im.h = Math.max(8, Math.round(im.h * factor));
    geoDirty = true;
    redraw();
  }

  // ---- 指针事件 ----
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
    const hit = pickImage(w.x, w.y);
    if (hit) {
      selectedImg = hit;
      dragImgMode = true;
      imgDragMoved = false;
      dragGrab = { x: w.x - hit.cx, y: w.y - hit.cy };
      redraw();
      return;
    }
    selectedImg = null;
    pointerDownStroke(Math.round(w.x), Math.round(w.y));
  });

  canvas.addEventListener('pointermove', (e) => {
    const p = pointerPos(e);
    const w = toWorld(p.x, p.y);
    if (panning) {
      viewport.x -= (p.x - lastX) / viewport.scale;
      viewport.y -= (p.y - lastY) / viewport.scale;
      lastX = p.x; lastY = p.y;
      requestVpSync();
      redraw();
      return;
    }
    if (dragImgMode && selectedImg) {
      selectedImg.cx = w.x - dragGrab.x;
      selectedImg.cy = w.y - dragGrab.y;
      imgDragMoved = true;
      geoDirty = true;
      redraw();
      return;
    }
    if (drawing && current) {
      pointerMoveStroke(Math.round(w.x), Math.round(w.y));
    }
  });

  function endPointer(e) {
    if (panning) { panning = false; return; }
    if (dragImgMode) {
      dragImgMode = false;
      dragGrab = null;
      flushPending();   // 立即发出最终几何，减少拖尾延迟
      return;
    }
    if (drawing && current && e) {
      const p = pointerPos(e);
      const w = toWorld(p.x, p.y);
      pointerMoveStroke(Math.round(w.x), Math.round(w.y));
    }
    pointerUpStroke();
  }
  canvas.addEventListener('pointerup', endPointer);
  canvas.addEventListener('pointercancel', endPointer);

  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.1 : 0.9;
    if (selectedImg) {
      zoomSelectedImg(factor);   // 选中图片时滚轮 = 缩放图片
      return;
    }
    const p = pointerPos(e);
    const w = toWorld(p.x, p.y);
    viewport.scale = clamp(viewport.scale * factor, 0.1, 10);
    viewport.x = w.x - p.x / viewport.scale;
    viewport.y = w.y - p.y / viewport.scale;
    requestVpSync();
    redraw();
  }, { passive: false });

  // ---- 键盘 ----
  document.addEventListener('keydown', (e) => {
    const mod = e.ctrlKey || e.metaKey;
    const k = e.key.toLowerCase();
    if (mod && k === 'z' && e.shiftKey) { e.preventDefault(); redo(); }
    else if (mod && k === 'z') { e.preventDefault(); undo(); }
    else if (mod && k === 'y') { e.preventDefault(); redo(); }
    else if ((e.key === 'Delete' || e.key === 'Backspace') && selectedImg) {
      e.preventDefault(); deleteSelectedImg();
    } else if (!mod && k === 'r' && selectedImg) {
      e.preventDefault(); rotateSelectedImg(Math.PI / 2);
    } else if (!mod && k === '[' && selectedImg) {
      e.preventDefault(); rotateSelectedImg(-Math.PI / 2);
    }
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
    if (!scene.length) return;
    pushOp({ kind: 'clear', snapshot: snapshotScene() });
    scene.length = 0;
    current = null;
    selectedImg = null;
    redraw();
    send({ type: 'clear' });
  });

  redraw();
})();
