/*
  Страницы грузятся отдельными чанками (React.lazy): гость по ссылке #/b, #/i, #/m
  не качает код приложения, а настройки (с QR-кодом) и админка не утяжеляют старт.
  Здесь — обёртка для именованных экспортов, экраны ожидания и перехват ошибки
  загрузки чанка (сеть пропала, после обновления сайта старый файл уже удалён).
*/
import { RefreshCw } from 'lucide-react';
import { Component, lazy, Suspense, type ComponentType, type ReactNode } from 'react';
import { Outlet, useLocation } from 'react-router-dom';
import { Spinner } from './ui';

/** React.lazy для именованного экспорта: lazyNamed(() => import('../pages/X'), 'X'). */
export function lazyNamed<M extends Record<string, unknown>, K extends keyof M>(load: () => Promise<M>, name: K) {
  return lazy(async () => ({ default: (await load())[name] as ComponentType }));
}

/** Ожидание внутри оболочки приложения: меню остаётся, на месте страницы — индикатор. */
export function PageLoading() {
  return (
    <div className="page-loading">
      <Spinner />
      <span className="muted small">Загружаем страницу…</span>
    </div>
  );
}

/** Ожидание на весь экран — для гостевых страниц (как их собственный экран загрузки). */
export function GateLoading({ text = 'Загружаем страницу…' }: { text?: string }) {
  return (
    <div className="gate">
      <div className="gate-card">
        <Spinner />
        <span className="muted small">{text}</span>
      </div>
    </div>
  );
}

interface BoundaryProps {
  children: ReactNode;
  full?: boolean;
  /** Сменился адрес — ошибку сбрасываем (без пересоздания самой страницы). */
  resetKey: string;
}

class LoadBoundary extends Component<BoundaryProps, { failed: boolean; key: string }> {
  state = { failed: false, key: this.props.resetKey };

  static getDerivedStateFromProps(props: BoundaryProps, state: { failed: boolean; key: string }) {
    return props.resetKey === state.key ? null : { failed: false, key: props.resetKey };
  }

  static getDerivedStateFromError() {
    return { failed: true };
  }

  render() {
    if (!this.state.failed) return this.props.children;
    // React.lazy запоминает неудачную загрузку — помогает только перезагрузка страницы.
    const body = (
      <div className="gate-card">
        <h1 className="title-md">Не удалось загрузить страницу</h1>
        <span className="muted small">Проверьте подключение к интернету и обновите страницу.</span>
        <button className="btn btn-secondary" onClick={() => location.reload()}>
          <RefreshCw size={15} /> Обновить
        </button>
      </div>
    );
    return this.props.full ? <div className="gate">{body}</div> : <div className="page-loading">{body}</div>;
  }
}

/**
  Ленивая страница: ошибка загрузки + ожидание. full — на весь экран (гость),
  иначе — внутри оболочки. На другом адресе ошибка сбрасывается.
*/
export function LazyPage({ children, full }: { children: ReactNode; full?: boolean }) {
  const { pathname, search } = useLocation();
  return (
    <LoadBoundary resetKey={`${pathname}${search}`} full={full}>
      <Suspense fallback={full ? <GateLoading /> : <PageLoading />}>{children}</Suspense>
    </LoadBoundary>
  );
}

/** Маршрут-обёртка без пути: все вложенные ленивые страницы — внутри LazyPage. */
export function LazyOutlet({ full }: { full?: boolean }) {
  return (
    <LazyPage full={full}>
      <Outlet />
    </LazyPage>
  );
}
