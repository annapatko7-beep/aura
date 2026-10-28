import { RefreshCw, WifiOff } from 'lucide-react';
import { useSession } from '../state/session';
import { ServerAddress } from './ServerAddress';
import { Logo, Spinner } from './ui';

/**
 * Экран ожидания: сервер ещё ни разу не ответил за этот запуск. Вместо белого экрана
 * или «Network Error» — понятный статус, «Повторить» и смена адреса.
 */
export function ConnectionScreen() {
  const { conn, connError, retry } = useSession();
  const searching = conn === 'connecting';
  return (
    <div className="gate">
      <div className="gate-card">
        <span className="logo-tile">
          <Logo />
        </span>
        <div className="row" style={{ justifyContent: 'center' }}>
          {searching ? <Spinner small /> : <WifiOff size={16} className="faint" />}
          <h1 className="title-md">{searching ? 'Поиск соединения с сервером Selfvoy…' : 'Сервер Selfvoy не отвечает'}</h1>
        </div>
        <p className="muted small" style={{ margin: 0 }}>
          {searching
            ? 'Подключаемся. Если сервер запущен на компьютере, телефон должен быть в той же сети.'
            : connError || 'Проверьте, что сервер запущен и адрес указан верно. Пробуем снова автоматически.'}
        </p>
        <button className="btn btn-primary" onClick={retry} disabled={searching}>
          <RefreshCw size={15} /> Повторить
        </button>
        <ServerAddress compact />
      </div>
    </div>
  );
}

/** Полоска поверх приложения, когда связь пропала после входа: работать можно, данные обновятся сами. */
export function OfflineBanner() {
  const { conn, retry } = useSession();
  if (conn === 'open') return null;
  return (
    <div className="banner" role="status">
      {conn === 'connecting' ? <Spinner small /> : <WifiOff size={14} />}
      <span>{conn === 'connecting' ? 'Переподключаемся к серверу Selfvoy…' : 'Нет связи с сервером — показываем последние данные'}</span>
      {conn !== 'connecting' && (
        <button className="link small" onClick={retry}>
          Повторить
        </button>
      )}
    </div>
  );
}
