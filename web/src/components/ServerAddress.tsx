import { useState } from 'react';
import { useSession } from '../state/session';
import { getPlatform } from '../platform';

/** Адрес сервера: показать текущий и поменять. Используется на экране ожидания, входе и в настройках. */
export function ServerAddress({ compact }: { compact?: boolean }) {
  const { serverUrl, setServer, status } = useSession();
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(serverUrl);
  const [error, setError] = useState('');
  const phone = getPlatform() === 'android' || getPlatform() === 'ios';

  const save = (e: React.FormEvent) => {
    e.preventDefault();
    const url = setServer(value);
    if (!url) {
      setError('Адрес вида ws://192.168.1.10:9000 или wss://selfvoy.example.com');
      return;
    }
    setError('');
    setEditing(false);
  };

  if (!editing) {
    const isStandalone = (serverUrl ?? '').includes('standalone');
    return (
      <div className={`row wrap ${compact ? 'small' : ''}`} style={{ justifyContent: 'center', gap: 6 }}>
        <span className="faint">Режим:</span>
        <span className="mono muted small">{isStandalone ? 'Автономный (встроенный)' : (serverUrl || '—')}</span>
        <button
          type="button"
          className="link small"
          onClick={() => {
            setValue(isStandalone ? '' : (serverUrl ?? ''));
            setEditing(true);
          }}
        >
          {isStandalone ? 'Подключить сервер' : 'Изменить'}
        </button>
      </div>
    );
  }

  return (
    <form className="stack" onSubmit={save} style={{ width: '100%', textAlign: 'left' }}>
      <label className="field">
        <span>Адрес сервера Selfvoy</span>
        <input
          className={`input mono ${error ? 'invalid' : ''}`}
          value={value}
          onChange={(e) => setValue(e.target.value)}
          placeholder="ws://192.168.1.10:9000"
          autoCapitalize="off"
          autoCorrect="off"
          spellCheck={false}
          inputMode="url"
          autoFocus
        />
        {error ? (
          <span className="field-error">{error}</span>
        ) : (
          <span className="hint">
            {phone
              ? 'На телефоне укажите IP компьютера с сервером в той же Wi-Fi сети или публичный wss-адрес. localhost здесь — сам телефон.'
              : 'Можно ввести IP или домен: для локальной сети подставится ws:// и порт 9000, для публичного домена — wss://; https:// превратится в wss://.'}
          </span>
        )}
      </label>
      {status === 'signedIn' && <div className="notice warn small">На другом сервере — другой аккаунт: после смены адреса нужно будет войти заново.</div>}
      <div className="row">
        <button className="btn btn-primary" type="submit">
          Подключиться
        </button>
        <button className="btn btn-ghost" type="button" onClick={() => setEditing(false)}>
          Отмена
        </button>
        <span className="spacer" />
        <button
          className="btn btn-ghost btn-sm"
          type="button"
          onClick={() => {
            setServer('selfvoy://standalone');
            setEditing(false);
          }}
        >
          Встроенный режим
        </button>
        <button
          className="btn btn-ghost btn-sm"
          type="button"
          onClick={() => {
            setServer(null);
            setEditing(false);
          }}
        >
          Сброс
        </button>
      </div>
    </form>
  );
}
