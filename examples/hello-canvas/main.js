/*
 * hello-canvas — the smallest complete game.
 *
 * Canvas 2D, keyboard and gamepad input, and a frame loop. If this runs, the
 * launcher is working: it is the first thing to try after downloading a binary.
 *
 * Plain browser APIs throughout — this same file runs unchanged in a browser.
 */

const canvas = document.getElementById('game-canvas');
const ctx = canvas.getContext('2d');
const W = canvas.width;
const H = canvas.height;

const player = { x: W / 2, y: H / 2, r: 18, speed: 260 };
const keys = new Set();

window.addEventListener('keydown', (e) => keys.add(e.code));
window.addEventListener('keyup', (e) => keys.delete(e.code));

// Mouse and pointer both work; either one moves the target.
let target = null;
window.addEventListener('pointerdown', (e) => { target = { x: e.clientX, y: e.clientY }; });

const sparkles = [];

function readInput() {
  let dx = 0, dy = 0;
  if (keys.has('ArrowLeft') || keys.has('KeyA')) dx -= 1;
  if (keys.has('ArrowRight') || keys.has('KeyD')) dx += 1;
  if (keys.has('ArrowUp') || keys.has('KeyW')) dy -= 1;
  if (keys.has('ArrowDown') || keys.has('KeyS')) dy += 1;

  // A connected gamepad drives the same movement, with a deadzone so a resting
  // stick does not creep.
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (const pad of pads) {
    if (!pad) continue;
    const ax = pad.axes[0] || 0, ay = pad.axes[1] || 0;
    if (Math.abs(ax) > 0.15) dx += ax;
    if (Math.abs(ay) > 0.15) dy += ay;
    if (pad.buttons[14]?.pressed) dx -= 1;
    if (pad.buttons[15]?.pressed) dx += 1;
    if (pad.buttons[12]?.pressed) dy -= 1;
    if (pad.buttons[13]?.pressed) dy += 1;
  }

  const len = Math.hypot(dx, dy);
  return len > 1 ? { dx: dx / len, dy: dy / len } : { dx, dy };
}

let last = 0;
function frame(now) {
  const dt = last ? Math.min((now - last) / 1000, 0.1) : 0;
  last = now;

  const { dx, dy } = readInput();
  player.x += dx * player.speed * dt;
  player.y += dy * player.speed * dt;

  if (target) {
    const tx = target.x - player.x, ty = target.y - player.y;
    const d = Math.hypot(tx, ty);
    if (d > 4) {
      player.x += (tx / d) * player.speed * dt;
      player.y += (ty / d) * player.speed * dt;
    } else {
      target = null;
    }
  }

  player.x = Math.max(player.r, Math.min(W - player.r, player.x));
  player.y = Math.max(player.r, Math.min(H - player.r, player.y));

  if ((dx || dy || target) && sparkles.length < 200) {
    sparkles.push({ x: player.x, y: player.y, life: 1 });
  }
  for (let i = sparkles.length - 1; i >= 0; i--) {
    sparkles[i].life -= dt * 1.5;
    if (sparkles[i].life <= 0) sparkles.splice(i, 1);
  }

  // --- draw ---------------------------------------------------------------
  ctx.fillStyle = '#0d1117';
  ctx.fillRect(0, 0, W, H);

  ctx.strokeStyle = '#1c2530';
  ctx.lineWidth = 1;
  for (let x = 0; x < W; x += 40) {
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  for (let y = 0; y < H; y += 40) {
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }

  for (const s of sparkles) {
    ctx.globalAlpha = Math.max(0, s.life) * 0.6;
    ctx.fillStyle = '#4fc3f7';
    ctx.beginPath();
    ctx.arc(s.x, s.y, 6 * s.life, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.globalAlpha = 1;

  ctx.fillStyle = '#ffd166';
  ctx.beginPath();
  ctx.arc(player.x, player.y, player.r, 0, Math.PI * 2);
  ctx.fill();
  ctx.strokeStyle = '#ffffff';
  ctx.lineWidth = 2;
  ctx.stroke();

  ctx.fillStyle = '#8b98a5';
  ctx.font = '16px sans-serif';
  ctx.fillText('WASD / arrows / gamepad to move, click to send', 16, 28);

  requestAnimationFrame(frame);
}

requestAnimationFrame(frame);
