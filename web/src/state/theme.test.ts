import { beforeEach, describe, expect, it } from 'vitest';
import {
  applyThemeAndDesign,
  DESIGN_OPTIONS,
  getSavedDesign,
  getSavedGlassStyle,
  getSavedTheme,
  initTheme,
  type DesignVariant,
  type GlassStyle,
} from './theme';

describe('theme & designs', () => {
  const store = new Map<string, string>();
  const attrs = new Map<string, string>();

  beforeEach(() => {
    store.clear();
    attrs.clear();

    (globalThis as unknown as { window: unknown }).window = globalThis;
    (globalThis as unknown as { localStorage: Storage }).localStorage = {
      getItem: (k: string) => store.get(k) ?? null,
      setItem: (k: string, v: string) => store.set(k, String(v)),
      removeItem: (k: string) => store.delete(k),
      clear: () => store.clear(),
      key: (i: number) => [...store.keys()][i] ?? null,
      get length() {
        return store.size;
      },
    };

    (globalThis as unknown as { document: unknown }).document = {
      documentElement: {
        setAttribute: (k: string, v: string) => attrs.set(k, String(v)),
        getAttribute: (k: string) => attrs.get(k) ?? null,
        removeAttribute: (k: string) => attrs.delete(k),
      },
      querySelector: () => null,
    };
  });

  it('содержит 9 проработанных тем оформления', () => {
    expect(DESIGN_OPTIONS.length).toBe(9);
    const ids = DESIGN_OPTIONS.map((d) => d.id);
    expect(ids).toEqual([
      'aurora',
      'minimal',
      'warm',
      'oled',
      'emerald',
      'cyberpunk',
      'midnight',
      'sunset',
      'lavender',
    ]);

    for (const opt of DESIGN_OPTIONS) {
      expect(opt.name).toBeTruthy();
      expect(opt.tagline).toBeTruthy();
      expect(opt.previewGradient).toBeTruthy();
      expect(opt.previewAccent).toBeTruthy();
    }
  });

  it('getSavedTheme возвращает сохранённую или system', () => {
    expect(getSavedTheme()).toBe('system');
    localStorage.setItem('selfvoy_theme', 'dark');
    expect(getSavedTheme()).toBe('dark');
    localStorage.setItem('selfvoy_theme', 'light');
    expect(getSavedTheme()).toBe('light');
    localStorage.setItem('selfvoy_theme', 'invalid');
    expect(getSavedTheme()).toBe('system');
  });

  it('getSavedDesign валидирует все 9 тем', () => {
    expect(getSavedDesign()).toBe('aurora');
    const allDesigns: DesignVariant[] = [
      'aurora',
      'minimal',
      'warm',
      'oled',
      'emerald',
      'cyberpunk',
      'midnight',
      'sunset',
      'lavender',
    ];
    for (const d of allDesigns) {
      localStorage.setItem('selfvoy_design', d);
      expect(getSavedDesign()).toBe(d);
    }
    localStorage.setItem('selfvoy_design', 'non_existing');
    expect(getSavedDesign()).toBe('aurora');
  });

  it('getSavedGlassStyle валидирует стили кнопок', () => {
    expect(getSavedGlassStyle()).toBe('glow');
    const styles: GlassStyle[] = ['glow', 'frosted', 'liquid'];
    for (const s of styles) {
      localStorage.setItem('selfvoy_glass_style', s);
      expect(getSavedGlassStyle()).toBe(s);
    }
    localStorage.setItem('selfvoy_glass_style', 'unknown');
    expect(getSavedGlassStyle()).toBe('glow');
  });

  it('applyThemeAndDesign выставляет атрибуты на documentElement', () => {
    applyThemeAndDesign('light', 'emerald', 'frosted');
    const root = (document as unknown as { documentElement: { getAttribute: (k: string) => string | null } }).documentElement;
    expect(root.getAttribute('data-theme')).toBe('light');
    expect(root.getAttribute('data-design')).toBe('emerald');
    expect(root.getAttribute('data-theme-mode')).toBe('light');
    expect(root.getAttribute('data-glass-style')).toBe('frosted');
  });

  it('OLED и Cyberpunk переключают на темную тему', () => {
    const root = (document as unknown as { documentElement: { getAttribute: (k: string) => string | null } }).documentElement;

    applyThemeAndDesign('light', 'oled', 'liquid');
    expect(root.getAttribute('data-theme')).toBe('dark');
    expect(root.getAttribute('data-design')).toBe('oled');

    applyThemeAndDesign('light', 'cyberpunk', 'glow');
    expect(root.getAttribute('data-theme')).toBe('dark');
    expect(root.getAttribute('data-design')).toBe('cyberpunk');
  });

  it('initTheme инициализирует тему из сохранённых настроек', () => {
    localStorage.setItem('selfvoy_design', 'sunset');
    localStorage.setItem('selfvoy_glass_style', 'liquid');
    localStorage.setItem('selfvoy_theme', 'dark');
    initTheme();
    const root = (document as unknown as { documentElement: { getAttribute: (k: string) => string | null } }).documentElement;
    expect(root.getAttribute('data-design')).toBe('sunset');
    expect(root.getAttribute('data-glass-style')).toBe('liquid');
    expect(root.getAttribute('data-theme')).toBe('dark');
  });
});
