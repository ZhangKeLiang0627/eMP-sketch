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
  ws.onopen = () => {
    statusEl.classList.add('online');
    statusText.textContent = '已连接';
    send({ type: 'sync' });          // 刷新/重连后拉取板端当前场景恢复画面
  };
  ws.onclose = () => { statusEl.classList.remove('online'); statusText.textContent = '已断开'; };
  ws.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (!msg || typeof msg.type !== 'string') return;
    if (msg.type === 'snapshot') applySnapshot(msg);
    else if (msg.type === 'draw') applyBoardStroke(msg);   // 板端触摸绘制的增量段
    else if (msg.type === 'clear') {
      scene.length = 0;
      history.length = 0;
      redoOps.length = 0;
      selectedImg = null;
      redraw();
      requestSync();   // 以板端权威（含撤销深度）校准
    }
  };
  const send = (o) => { if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o)); };

  // 板端广播的笔画段：同 id 段合并进本地末笔（与板端 addStroke 语义一致），不入本地撤销历史
  function applyBoardStroke(msg) {
    let tail = null;
    for (let i = scene.length - 1; i >= 0; --i) {
      const it = scene[i];
      if (it.kind === 'stroke' && it.s.id === msg.id) { tail = it.s; break; }
    }
    if (!tail) {
      scene.push({ kind: 'stroke', s: {
        id: msg.id, brush: msg.brush || 'pen', alpha: msg.alpha ?? 255,
        color: msg.color, width: msg.width, points: (msg.points || []).slice() } });
    } else {
      const p = tail.points, n = msg.points || [];
      if (n.length >= 2 && p.length >= 2 && p[p.length - 2] === n[0] && p[p.length - 1] === n[1]) {
        tail.points.push(...n.slice(2));
      } else {
        tail.points.push(...n);
      }
    }
    redraw();
  }

  // 板端撤销/重做深度（刷新后本地无历史时，用板端深度决定 Ctrl+Z 是否可回退）
  let serverUndo = 0, serverRedo = 0;

  // 快照恢复：用板端场景全量重建（RGB565 位图 → 本地 RGBA canvas）
  function rgb565ToCanvas(w, h, b64) {
    const bin = atob(b64);
    const cv = document.createElement('canvas');
    cv.width = w; cv.height = h;
    const cctx = cv.getContext('2d');
    const img = cctx.createImageData(w, h);
    for (let i = 0; i < w * h; ++i) {
      const lo = bin.charCodeAt(i * 2), hi = bin.charCodeAt(i * 2 + 1);
      const c16 = lo | (hi << 8);
      img.data[i * 4] = ((c16 >> 11) & 0x1F) << 3;
      img.data[i * 4 + 1] = ((c16 >> 5) & 0x3F) << 2;
      img.data[i * 4 + 2] = (c16 & 0x1F) << 3;
      img.data[i * 4 + 3] = 255;
    }
    cctx.putImageData(img, 0, 0);
    return cv;
  }
  function applySnapshot(msg) {
    scene.length = 0;
    bitmaps.clear();
    history.length = 0;
    redoOps.length = 0;
    selectedImg = null;
    viewport = { scale: 1, x: 0, y: 0 };
    if (msg.vp) viewport = { scale: msg.vp.scale, x: msg.vp.x, y: msg.vp.y };
    let maxId = 1000;
    for (const b of msg.imgs || []) {
      bitmaps.set(b.img, rgb565ToCanvas(b.w, b.h, b.data));
    }
    for (const it of msg.items || []) {
      if (it.k === 0) {
        scene.push({ kind: 'stroke', s: { id: it.id, brush: it.brush, alpha: it.alpha,
                                          color: it.color, width: it.width, points: it.pts } });
      } else {
        scene.push({ kind: 'img', im: { id: it.id, img: it.img, cx: it.cx, cy: it.cy,
                                        w: it.w, h: it.h, rot: it.rot } });
      }
      maxId = Math.max(maxId, it.id);
    }
    itemSeq = Math.max(itemSeq, maxId);
    serverUndo = msg.undo ?? 0;
    serverRedo = msg.redo ?? 0;
    redraw();
  }

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
  // ---- 图片选中框 + 手柄（角=等比缩放 / 边中=单轴拉伸 / 顶部圆=旋转）----
  function imgHandles(im) {
    const cos = Math.cos(im.rot), sin = Math.sin(im.rot);
    const hw = im.w / 2, hh = im.h / 2;
    const P = (lx, ly) => ({ x: im.cx + lx * cos - ly * sin, y: im.cy + lx * sin + ly * cos });
    const corners = [P(-hw, -hh), P(hw, -hh), P(hw, hh), P(-hw, hh)];
    const edges = [P(0, -hh), P(hw, 0), P(0, hh), P(-hw, 0)];   // 上 右 下 左
    const off = 18 / viewport.scale;
    const rot = P(0, -(hh + off));
    return { corners, edges, rot };
  }
  function drawSelectionOverlay() {
    if (!selectedImg) return;
    const im = selectedImg;
    const Hd = imgHandles(im);
    ctx.save();
    ctx.strokeStyle = '#5B6AF0';
    ctx.lineWidth = 1.2 / viewport.scale;
    ctx.setLineDash([4 / viewport.scale, 3 / viewport.scale]);
    ctx.beginPath();
    Hd.corners.forEach((p, i) => (i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
    ctx.closePath();
    ctx.stroke();
    ctx.setLineDash([]);
    // 旋转手柄杆 + 圆
    ctx.beginPath();
    ctx.moveTo(Hd.edges[0].x, Hd.edges[0].y);
    ctx.lineTo(Hd.rot.x, Hd.rot.y);
    ctx.stroke();
    ctx.fillStyle = '#5B6AF0';
    ctx.beginPath();
    ctx.arc(Hd.rot.x, Hd.rot.y, 4.5 / viewport.scale, 0, Math.PI * 2);
    ctx.fill();
    // 8 个缩放手柄：角 = 方块，边中 = 圆
    const hs = 3.5 / viewport.scale;
    Hd.corners.forEach((p) => {
      ctx.fillStyle = '#FFFFFF';
      ctx.strokeStyle = '#5B6AF0';
      ctx.lineWidth = 1.2 / viewport.scale;
      ctx.fillRect(p.x - hs, p.y - hs, hs * 2, hs * 2);
      ctx.strokeRect(p.x - hs, p.y - hs, hs * 2, hs * 2);
    });
    Hd.edges.forEach((p) => {
      ctx.fillStyle = '#FFFFFF';
      ctx.strokeStyle = '#5B6AF0';
      ctx.beginPath();
      ctx.arc(p.x, p.y, hs * 0.9, 0, Math.PI * 2);
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
  function requestSync() { if (ws.readyState === WebSocket.OPEN) send({ type: 'sync' }); }
  function undo() {
    if (drawing || dragImgMode || adj) return;
    if (!history.length) {
      // 刷新后本地无历史：退回板端历史（发 undo 后拉快照重建）
      if (serverUndo > 0) { serverUndo = 0; send({ type: 'undo' }); requestSync(); }
      return;
    }
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
    if (drawing || dragImgMode || adj) return;
    if (!redoOps.length) {
      if (serverRedo > 0) { serverRedo = 0; send({ type: 'redo' }); requestSync(); }
      return;
    }
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

  // ---- 图片：命中/选中/拖拽/手柄调整 ----
  let selectedImg = null;
  let dragImgMode = false;     // 拖动整图
  let dragGrab = null;         // 拾取点相对图片中心的偏移（世界坐标）
  let imgDragMoved = false;
  let adj = null;              // 手柄调整：{kind:'scale'|'stretch'|'rotate', start...}

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
  // 命中选中图片的手柄：返回 null（未命中）或 {kind, i}
  function hitHandle(wx, wy) {
    if (!selectedImg) return null;
    const tol = 14 / viewport.scale;
    const Hd = imgHandles(selectedImg);
    const R = (p) => Math.hypot(p.x - wx, p.y - wy) <= tol;
    if (R(Hd.rot)) return { kind: 'rotate' };
    for (let i = 0; i < 4; ++i) if (R(Hd.corners[i])) return { kind: 'scale', i };
    for (let i = 0; i < 4; ++i) if (R(Hd.edges[i])) return { kind: 'stretch', i };
    return null;
  }
  function beginAdjust(kind, i, wx, wy) {
    const im = selectedImg;
    adj = {
      kind, i,
      start: { cx: im.cx, cy: im.cy, w: im.w, h: im.h, rot: im.rot },
      center: { x: im.cx, y: im.cy },
      press: { x: wx, y: wy },
      startDist: Math.max(1, Math.hypot(wx - im.cx, wy - im.cy)),
      startAngle: Math.atan2(wy - im.cy, wx - im.cx),
    };
  }
  function applyAdjust(wx, wy) {
    const im = selectedImg;
    if (!im || !adj) return;
    const st = adj.start;
    if (adj.kind === 'scale') {
      const d = Math.hypot(wx - st.cx, wy - st.cy);
      const f = clamp(d / st.startDist, 0.05, 40);
      im.w = Math.max(8, st.w * f);
      im.h = Math.max(8, st.h * f);
      im.cx = st.cx; im.cy = st.cy;
    } else if (adj.kind === 'stretch') {
      // 局部轴投影：上/下边改 h，左/右边改 w（中心锚定）
      const dx = wx - st.cx, dy = wy - st.cy;
      const cos = Math.cos(-st.rot), sin = Math.sin(-st.rot);
      const lx = dx * cos - dy * sin, ly = dx * sin + dy * cos;
      if (adj.i === 0 || adj.i === 2) {           // 上 / 下 → 高
        const nh = Math.max(8, Math.abs(ly) * 2);
        im.h = (Math.abs(ly) < 1e-6) ? st.h : nh;
        im.w = st.w;
      } else {                                    // 左 / 右 → 宽
        const nw = Math.max(8, Math.abs(lx) * 2);
        im.w = (Math.abs(lx) < 1e-6) ? st.w : nw;
        im.h = st.h;
      }
      im.cx = st.cx; im.cy = st.cy;
    } else {   // rotate：绕中心任意角
      const a = Math.atan2(wy - st.cy, wx - st.cx);
      im.rot = st.rot + (a - st.startAngle);
    }
    geoDirty = true;
    redraw();
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

  // 工具栏"图片"：本地文件导入（与粘贴同一管线）
  const fileInput = document.createElement('input');
  fileInput.type = 'file';
  fileInput.accept = 'image/*';
  fileInput.style.display = 'none';
  document.body.appendChild(fileInput);
  const importBtn = document.getElementById('import-img');
  if (importBtn) {
    importBtn.addEventListener('click', () => fileInput.click());
  }
  fileInput.addEventListener('change', () => {
    const f = fileInput.files && fileInput.files[0];
    if (!f) return;
    const url = URL.createObjectURL(f);
    const imgEl = new Image();
    imgEl.onload = () => { importImage(imgEl); URL.revokeObjectURL(url); };
    imgEl.src = url;
    fileInput.value = '';
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

  // ---- 文字工具：点击画布 → 输入文本 → 栅格化为图片对象（天然支持中文） ----
  function addTextAt(wx, wy) {
    const txt = window.prompt('输入文字（确定上屏）', '');
    if (txt == null) { selectedImg = null; redraw(); return; }
    const t = txt.trim();
    if (!t) { selectedImg = null; redraw(); return; }
    const fs = 30;
    const fam = '"PingFang SC","Microsoft YaHei","Noto Sans CJK SC",sans-serif';
    const tmp = document.createElement('canvas');
    const mctx = tmp.getContext('2d');
    mctx.font = `${fs}px ${fam}`;
    const tw = Math.ceil(mctx.measureText(t).width);
    const th = Math.ceil(fs * 1.35);
    tmp.width = Math.max(2, tw + 10);
    tmp.height = Math.max(2, th);
    const cctx = tmp.getContext('2d');
    cctx.font = `${fs}px ${fam}`;
    cctx.textBaseline = 'alphabetic';
    cctx.fillStyle = color;
    cctx.fillText(t, 5, fs + 3);

    const cw = tmp.width, ch = tmp.height;
    const data = cctx.getImageData(0, 0, cw, ch).data;
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
    const imgId = nextImgId();
    send({ type: 'img-data', img: imgId, w: cw, h: ch, data: btoa(bin) });

    const src = document.createElement('canvas');
    src.width = cw; src.height = ch;
    src.getContext('2d').drawImage(tmp, 0, 0);
    bitmaps.set(imgId, src);

    const im = {
      id: nextItemId(), img: imgId,
      cx: wx + cw / 2, cy: wy + ch / 2, w: cw, h: ch, rot: 0,
    };
    scene.push({ kind: 'img', im });
    pushOp({ kind: 'add-img', id: im.id, im });
    selectedImg = im;
    send({ type: 'img', id: im.id, img: im.img, cx: im.cx, cy: im.cy, w: im.w, h: im.h, rot: im.rot });
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
    if (tool === 'text') {
      addTextAt(Math.round(w.x), Math.round(w.y));
      return;
    }
    const hh = hitHandle(w.x, w.y);
    if (hh) {
      beginAdjust(hh.kind, hh.i, w.x, w.y);
      return;
    }
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
    if (adj && selectedImg) {
      applyAdjust(w.x, w.y);
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
    if (adj) {
      adj = null;
      flushPending();   // 立即发出最终几何
      return;
    }
    if (dragImgMode) {
      dragImgMode = false;
      dragGrab = null;
      flushPending();
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
