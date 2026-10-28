export type ThemeMode = 'system' | 'light' | 'dark';
export type DesignVariant = 'aurora' | 'minimal' | 'warm' | 'oled' | 'emerald' | 'cyberpunk' | 'midnight' | 'sunset' | 'lavender';
export type GlassStyle = 'glow' | 'frosted' | 'liquid';

export interface DesignOption {
  id: DesignVariant;
  name: string;
  tagline: string;
  previewGradient: string;
  previewGlass: string;
  previewBg: string;
  previewAccent: string;
  badgeLabel?: string;
  buttonGradient?: string;
}

export const DESIGN_OPTIONS: readonly DesignOption[] = [
  {
    id: 'aurora',
    name: 'Сияние iOS',
    tagline: 'Фирменное неоновое сияние, мягкие градиенты и эффект жидкого стекла iOS 18',
    previewGradient: 'linear-gradient(135deg, #3d6bff, #6a4cff 52%, #b04cff)',
    previewGlass: 'rgba(255, 255, 255, 0.65)',
    previewBg: '#f4f3fa',
    previewAccent: '#5b4cff',
    badgeLabel: 'iOS 18 Glass',
    buttonGradient: 'linear-gradient(135deg, #5b4cff, #9b51e0)',
  },
  {
    id: 'minimal',
    name: 'Минимализм Pro',
    tagline: 'Строгий монохром, максимальная контрастность и чистые линии без отвлекающего фона',
    previewGradient: 'linear-gradient(135deg, #2b3a67, #3b5bfd 60%, #5b79ff)',
    previewGlass: 'rgba(255, 255, 255, 0.9)',
    previewBg: '#f6f6f9',
    previewAccent: '#3b5bfd',
    badgeLabel: 'Monochrome',
    buttonGradient: 'linear-gradient(135deg, #2b3a67, #3b5bfd)',
  },
  {
    id: 'warm',
    name: 'Тёплый янтарь',
    tagline: 'Уютная палитра книжной бумаги, тёплый солнечный янтарь и мягкий свет для глаз',
    previewGradient: 'linear-gradient(135deg, #e07a00, #f59e0b 50%, #fcd34d)',
    previewGlass: 'rgba(255, 250, 240, 0.75)',
    previewBg: '#faf7f2',
    previewAccent: '#d97706',
    badgeLabel: 'Paper warm',
    buttonGradient: 'linear-gradient(135deg, #d97706, #f59e0b)',
  },
  {
    id: 'oled',
    name: 'Глубокий космос',
    tagline: 'Абсолютный чёрный OLED-фон, светящиеся стеклянные кнопки с неоновыми гранями',
    previewGradient: 'linear-gradient(135deg, #7c3aed, #a855f7 55%, #06b6d4)',
    previewGlass: 'rgba(255, 255, 255, 0.12)',
    previewBg: '#040407',
    previewAccent: '#a855f7',
    badgeLabel: 'OLED Black',
    buttonGradient: 'linear-gradient(135deg, rgba(168, 85, 247, 0.85), rgba(6, 182, 212, 0.85))',
  },
  {
    id: 'emerald',
    name: 'Изумрудный Нефрит',
    tagline: 'Глубокий хвойно-изумрудный тон, мятное неоновое свечение и гармония для уставших глаз',
    previewGradient: 'linear-gradient(135deg, #064e3b, #059669 48%, #10b981 78%, #34d399)',
    previewGlass: 'rgba(255, 255, 255, 0.65)',
    previewBg: '#f0fdf4',
    previewAccent: '#059669',
    badgeLabel: 'Emerald Mint',
    buttonGradient: 'linear-gradient(135deg, #059669, #10b981)',
  },
  {
    id: 'cyberpunk',
    name: 'Неоновый Киберпанк',
    tagline: 'Энергичный контраст фуксии, неонового розе и электрического ультрамарина',
    previewGradient: 'linear-gradient(135deg, #ec4899, #d946ef 52%, #8b5cf6)',
    previewGlass: 'rgba(255, 255, 255, 0.15)',
    previewBg: '#090714',
    previewAccent: '#ec4899',
    badgeLabel: 'Cyber Neon',
    buttonGradient: 'linear-gradient(135deg, #ec4899, #8b5cf6)',
  },
  {
    id: 'midnight',
    name: 'Полуночный Сапфир',
    tagline: 'Королевский глубокий индиго-синий, сапфировый блеск стекла и кристальная чёткость',
    previewGradient: 'linear-gradient(135deg, #0f172a, #1e3a8a 45%, #2563eb 80%, #38bdf8)',
    previewGlass: 'rgba(255, 255, 255, 0.2)',
    previewBg: '#0a0f1d',
    previewAccent: '#2563eb',
    badgeLabel: 'Sapphire Blue',
    buttonGradient: 'linear-gradient(135deg, #1e40af, #3b82f6)',
  },
  {
    id: 'sunset',
    name: 'Калифорнийский Закат',
    tagline: 'Тёплый закатный бриз, кораллово-розовый градиент и мягкий бархатный свет',
    previewGradient: 'linear-gradient(135deg, #f43f5e, #fb7185 45%, #fb923c)',
    previewGlass: 'rgba(255, 245, 245, 0.75)',
    previewBg: '#fff5f5',
    previewAccent: '#f43f5e',
    badgeLabel: 'Sunset Coral',
    buttonGradient: 'linear-gradient(135deg, #e11d48, #f97316)',
  },
  {
    id: 'lavender',
    name: 'Прованская Лаванда',
    tagline: 'Нежные пастельные оттенки лаванды, матовое дымчатое стекло и спокойный ритм',
    previewGradient: 'linear-gradient(135deg, #6d28d9, #8b5cf6 50%, #c084fc 80%, #e9d5ff)',
    previewGlass: 'rgba(255, 255, 255, 0.75)',
    previewBg: '#f7f4fd',
    previewAccent: '#8b5cf6',
    badgeLabel: 'Soft Lavender',
    buttonGradient: 'linear-gradient(135deg, #7c3aed, #a78bfa)',
  },
] as const;

const THEME_KEY = 'selfvoy_theme';
const DESIGN_KEY = 'selfvoy_design';
const GLASS_KEY = 'selfvoy_glass_style';

const VALID_DESIGNS = new Set<string>([
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

function getSystemPrefersDark(): boolean {
  if (typeof window === 'undefined' || !window.matchMedia) return false;
  return window.matchMedia('(prefers-color-scheme: dark)').matches;
}

export function getSavedTheme(): ThemeMode {
  if (typeof localStorage === 'undefined') return 'system';
  const val = localStorage.getItem(THEME_KEY);
  if (val === 'light' || val === 'dark' || val === 'system') return val;
  return 'system';
}

export function getSavedDesign(): DesignVariant {
  if (typeof localStorage === 'undefined') return 'aurora';
  const val = localStorage.getItem(DESIGN_KEY);
  if (val && VALID_DESIGNS.has(val)) return val as DesignVariant;
  return 'aurora';
}

export function getSavedGlassStyle(): GlassStyle {
  if (typeof localStorage === 'undefined') return 'glow';
  const val = localStorage.getItem(GLASS_KEY);
  if (val === 'glow' || val === 'frosted' || val === 'liquid') return val;
  return 'glow';
}

export function applyThemeAndDesign(theme: ThemeMode, design: DesignVariant, glassStyle: GlassStyle = getSavedGlassStyle()) {
  if (typeof document === 'undefined') return;
  const root = document.documentElement;

  // Режим OLED всегда требует тёмного окружения для максимального контраста и эффекта
  const effectiveTheme = design === 'oled' || design === 'cyberpunk'
    ? 'dark'
    : theme === 'system'
      ? (getSystemPrefersDark() ? 'dark' : 'light')
      : theme;

  root.setAttribute('data-theme', effectiveTheme);
  root.setAttribute('data-design', design);
  root.setAttribute('data-theme-mode', theme);
  root.setAttribute('data-glass-style', glassStyle);

  // Meta theme-color для браузера и статусной строки мобильных устройств
  const meta = document.querySelector('meta[name="theme-color"]');
  if (meta) {
    let color = '#f4f3fa';
    if (design === 'oled') color = '#040407';
    else if (design === 'cyberpunk') color = '#090714';
    else if (design === 'midnight') color = '#0a0f1d';
    else if (design === 'emerald') color = effectiveTheme === 'dark' ? '#061a12' : '#f0fdf4';
    else if (design === 'sunset') color = effectiveTheme === 'dark' ? '#1c1012' : '#fff5f5';
    else if (design === 'lavender') color = effectiveTheme === 'dark' ? '#120f1c' : '#f7f4fd';
    else if (effectiveTheme === 'dark') {
      if (design === 'warm') color = '#151311';
      else if (design === 'minimal') color = '#111114';
      else color = '#09080f';
    } else {
      if (design === 'warm') color = '#faf7f2';
      else if (design === 'minimal') color = '#f6f6f9';
    }
    meta.setAttribute('content', color);
  }
}

let listenerAttached = false;

export function initTheme() {
  if (typeof window === 'undefined') return;
  const theme = getSavedTheme();
  const design = getSavedDesign();
  const glassStyle = getSavedGlassStyle();
  applyThemeAndDesign(theme, design, glassStyle);

  if (!listenerAttached && window.matchMedia) {
    listenerAttached = true;
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
      const currentTheme = getSavedTheme();
      if (currentTheme === 'system') {
        applyThemeAndDesign('system', getSavedDesign(), getSavedGlassStyle());
        window.dispatchEvent(new CustomEvent('selfvoy_theme_change'));
      }
    });
  }
}

import { useEffect, useState } from 'react';

export function useTheme() {
  const [theme, setThemeState] = useState<ThemeMode>(getSavedTheme);
  const [design, setDesignState] = useState<DesignVariant>(getSavedDesign);
  const [glassStyle, setGlassStyleState] = useState<GlassStyle>(getSavedGlassStyle);

  useEffect(() => {
    const onThemeChange = () => {
      setThemeState(getSavedTheme());
      setDesignState(getSavedDesign());
      setGlassStyleState(getSavedGlassStyle());
    };
    window.addEventListener('selfvoy_theme_change', onThemeChange);
    return () => window.removeEventListener('selfvoy_theme_change', onThemeChange);
  }, []);

  const setTheme = (next: ThemeMode) => {
    try {
      localStorage.setItem(THEME_KEY, next);
    } catch {}
    setThemeState(next);
    applyThemeAndDesign(next, design, glassStyle);
    window.dispatchEvent(new CustomEvent('selfvoy_theme_change'));
  };

  const setDesign = (next: DesignVariant) => {
    try {
      localStorage.setItem(DESIGN_KEY, next);
    } catch {}
    setDesignState(next);
    applyThemeAndDesign(theme, next, glassStyle);
    window.dispatchEvent(new CustomEvent('selfvoy_theme_change'));
  };

  const setGlassStyle = (next: GlassStyle) => {
    try {
      localStorage.setItem(GLASS_KEY, next);
    } catch {}
    setGlassStyleState(next);
    applyThemeAndDesign(theme, design, next);
    window.dispatchEvent(new CustomEvent('selfvoy_theme_change'));
  };

  return { theme, design, glassStyle, setTheme, setDesign, setGlassStyle };
}
