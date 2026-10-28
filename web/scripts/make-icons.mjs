// Renders the Selfvoy swirl into the source images that `capacitor-assets` turns into every
// iOS/Android icon and splash size, plus build/icon.png for the desktop installers.
import { mkdirSync } from 'node:fs';
import sharp from 'sharp';

const DASHES = 12;

function swirl(cx, cy, r, stroke) {
  // Same geometry as SelfvoyMark in src/components/ui.tsx, scaled from its 100×100 box.
  const k = r / 41;
  const p = (x, y) => `${cx + (x - 50) * k} ${cy + (y - 50) * k}`;
  return Array.from({ length: DASHES }, (_, i) => `<path d="M${p(50, 9)} Q ${p(60, 9.5)} ${p(66, 19)}" transform="rotate(${(360 / DASHES) * i} ${cx} ${cy})"/>`)
    .join('')
    .replace(/^/, `<g fill="none" stroke="url(#g)" stroke-width="${stroke}" stroke-linecap="round">`)
    .concat('</g>');
}

const defs = `
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#f6e6ff"/><stop offset="1" stop-color="#b18cff"/>
    </linearGradient>
    <radialGradient id="bg" cx="0.5" cy="0.42" r="0.75">
      <stop offset="0" stop-color="#3b1d78"/><stop offset="0.55" stop-color="#160f2b"/><stop offset="1" stop-color="#09080e"/>
    </radialGradient>
    <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="18" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
  </defs>`;

const svg = (w, h, body) => Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${defs}${body}</svg>`);

const icon = svg(1024, 1024, `<rect width="1024" height="1024" fill="url(#bg)"/><g filter="url(#glow)">${swirl(512, 512, 300, 38)}</g>`);
// Android adaptive icons crop to the inner ~66%, so the foreground mark is smaller.
const foreground = svg(1024, 1024, `<g filter="url(#glow)">${swirl(512, 512, 230, 30)}</g>`);
const background = svg(1024, 1024, `<rect width="1024" height="1024" fill="url(#bg)"/>`);
const splash = svg(2732, 2732, `<rect width="2732" height="2732" fill="#09080e"/><g filter="url(#glow)">${swirl(1366, 1366, 210, 26)}</g>`);

mkdirSync('assets', { recursive: true });
mkdirSync('build', { recursive: true });
await Promise.all([
  sharp(icon).png().toFile('assets/icon-only.png'),
  sharp(icon).png().toFile('build/icon.png'),
  sharp(foreground).png().toFile('assets/icon-foreground.png'),
  sharp(background).png().toFile('assets/icon-background.png'),
  sharp(splash).png().toFile('assets/splash.png'),
  sharp(splash).png().toFile('assets/splash-dark.png'),
]);
console.log('icons: assets/*.png, build/icon.png');
