/*
  Настройки → «Подключения» и тариф:
  - Telegram (F2): telegram.status / link / unlink — уведомления о записях в личный чат;
  - «Календарь в телефоне» (F6): calendar.feed / feedReset / feedDisable — ICS-подписка;
  - ЮKassa (F4): payments.status / connect / disconnect — предоплата картой и СБП;
  - тариф (F3): user.plan.

  Секретный ключ ЮKassa живёт только в поле ввода до отправки: не сохраняется на
  устройстве и не пишется в журнал; после подключения поле очищается.
*/
import { Bell, CalendarDays, Copy, CreditCard, ExternalLink, Gift, RefreshCw, Send } from 'lucide-react';
import { useCallback, useEffect, useMemo, useState, type FormEvent } from 'react';
import { Link } from 'react-router-dom';
import { Group, PasswordInput, ProBadge, Spinner, StatusPill } from '../components/ui';
import { copyText } from '../lib/clipboard';
import { when } from '../lib/format';
import { icsFeedUrl, isProRequired, planView, PRO_REQUIRED_TEXT, webcalUrl } from '../lib/integrations';
import { makeIntegrationsApi, type CalendarFeed, type PaymentsStatus, type TelegramStatus } from '../net/integrationsApi';
import { pushService } from '../net/pushService';
import { isNetworkError } from '../net/socket';
import { useData } from '../state/data';
import { errorText, useSession } from '../state/session';

function useIntegrations() {
  const { socket } = useSession();
  return useMemo(() => makeIntegrationsApi(socket), [socket]);
}

const failText = (e: unknown) => (isProRequired(e) ? PRO_REQUIRED_TEXT : errorText(e));

// ------------------------------------------------------------------ тариф

export function PlanCard() {
  const { user } = useSession();
  const plan = planView(user?.plan);
  if (!plan) return null;
  return (
    <Group head="Тариф" foot={plan.hint}>
      <div className="cell has-ico">
        <span className={`ico ${plan.pro ? 'ink' : ''}`}>
          <Gift size={16} />
        </span>
        <span className="lab">
          <span className="row" style={{ gap: 8 }}>
            {plan.label} {plan.pro && <ProBadge />}
          </span>
          <small>Месяц Pro — за каждого коллегу, который заведёт Selfvoy</small>
        </span>
        <Link to="/settings?s=referral" className="btn btn-secondary btn-sm">
          Пригласить
        </Link>
      </div>
    </Group>
  );
}

// ------------------------------------------------------------------ Push-уведомления

export function PushNotificationsSection() {
  const { api } = useSession();
  const { notify } = useData();
  const [granted, setGranted] = useState(false);
  const [busy, setBusy] = useState(false);
  const [isNative, setIsNative] = useState(false);

  useEffect(() => {
    try {
      setIsNative(pushService.isNative());
      void pushService.getPermissionStatus().then(
        (status) => setGranted(status === 'granted'),
        () => setGranted(false),
      );
    } catch {
      setGranted(false);
    }
  }, []);

  const handleEnable = async () => {
    setBusy(true);
    try {
      const res = await pushService.requestPermissionAndRegister(api);
      if (res.permission === 'granted') {
        setGranted(true);
        notify('Push-уведомления успешно включены!');
        pushService.showLocalNotification('Selfvoy', 'Уведомления о встречах активированы');
      } else {
        notify(res.error || 'Не удалось включить push-уведомления');
      }
    } catch (e) {
      notify(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const handleTest = () => {
    pushService.showLocalNotification(
      'Напоминание о встрече',
      'Завтра в 15:00: Встреча с Alex Morgan (Selfvoy Calendar)',
      '/booking'
    );
    notify('Тестовое push-уведомление отправлено на устройство!');
  };

  return (
    <Group head="Уведомления на телефоне (Push)" foot="Системные push-уведомления о новых встречах, изменениях слотов и напоминаниях.">
      <div className="cell has-ico">
        <span className={`ico ${granted ? 'ink' : ''}`}>
          <Bell size={16} />
        </span>
        <span className="lab">
          <span className="row" style={{ gap: 8 }}>
            <span>{isNative ? 'Мобильный Push (FCM Android/iOS)' : 'Системные Push-уведомления'}</span>
            <StatusPill status={{ label: granted ? 'Включены' : 'Выключены', tone: granted ? 'green' : '', icon: granted ? 'check' : 'minus' }} />
          </span>
          <small>{granted ? 'Устройство подключено к шлюзу уведомлений' : 'Разрешите уведомления, чтобы не пропустить встречи'}</small>
        </span>
        <div className="row" style={{ gap: 6 }}>
          {granted ? (
            <button type="button" className="btn btn-secondary btn-sm" onClick={handleTest}>
              Проверить
            </button>
          ) : (
            <button type="button" className="btn btn-primary btn-sm" onClick={handleEnable} disabled={busy}>
              {busy ? <Spinner small /> : 'Включить'}
            </button>
          )}
        </div>
      </div>
    </Group>
  );
}

// ------------------------------------------------------------------ Telegram

export function TelegramSection() {
  const api = useIntegrations();
  const { conn } = useSession();
  const { notify } = useData();
  const [status, setStatus] = useState<TelegramStatus | null>(null);
  const [link, setLink] = useState<{ url: string; expires_at: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    try {
      const s = await api.telegramStatus();
      setStatus(s);
      if (s.linked) setLink(null);
      setError('');
    } catch (e) {
      if (!isNetworkError(e)) setError(failText(e));
    }
  }, [api]);

  useEffect(() => {
    if (conn === 'open') void load();
  }, [conn, load]);

  // Вернулись из Telegram — проверяем, привязался ли чат.
  useEffect(() => {
    const onFocus = () => {
      if (link) void load();
    };
    window.addEventListener('focus', onFocus);
    return () => window.removeEventListener('focus', onFocus);
  }, [link, load]);

  const connect = async () => {
    setBusy(true);
    setError('');
    try {
      const reply = await api.telegramLink();
      setLink(reply);
      // Окно могли заблокировать — тогда остаётся ссылка ниже.
      window.open(reply.url, '_blank', 'noopener,noreferrer');
    } catch (e) {
      setError(failText(e));
    } finally {
      setBusy(false);
    }
  };

  const unlink = async () => {
    setBusy(true);
    try {
      await api.telegramUnlink();
      notify('Telegram отключён');
      await load();
    } catch (e) {
      setError(failText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Group head="Telegram" foot={status?.enabled === false ? undefined : 'Новые записи, переносы, отмены, «Приду» и оплаты — сообщением в Telegram. Отключить можно и командой /stop в чате с ботом.'}>
      <div className="cell has-ico">
        <span className="ico tg">
          <Send size={16} />
        </span>
        <span className="lab">
          Telegram
          <small>
            {status === null
              ? 'Проверяем…'
              : !status.enabled
                ? 'Бот не настроен на сервере'
                : status.linked
                  ? `Подключено${status.username ? ` · @${status.username}` : ''}`
                  : status.bot_username
                    ? `Бот @${status.bot_username}`
                    : 'Не подключено'}
          </small>
        </span>
        {status === null ? (
          <Spinner small />
        ) : !status.enabled ? (
          <StatusPill status={{ label: 'Недоступно', tone: '', icon: 'minus' }} />
        ) : status.linked ? (
          <button type="button" className="btn btn-danger btn-sm" disabled={busy} onClick={unlink}>
            Отключить
          </button>
        ) : (
          <button type="button" className="btn btn-primary btn-sm" disabled={busy} onClick={connect}>
            {busy ? <Spinner small /> : 'Подключить'}
          </button>
        )}
      </div>
      {link && !status?.linked && (
        <div className="cell stacked">
          <span className="small">
            Откройте бота и нажмите «Запустить» (Start) — затем вернитесь сюда. Ссылка действует до {when(link.expires_at)}.
          </span>
          <div className="row wrap" style={{ gap: 8 }}>
            <a className="btn btn-secondary btn-sm" href={link.url} target="_blank" rel="noopener noreferrer">
              <ExternalLink size={14} /> Открыть Telegram
            </a>
            <button type="button" className="btn btn-ghost btn-sm" onClick={() => void load()}>
              <RefreshCw size={14} /> Проверить
            </button>
          </div>
        </div>
      )}
      {error && (
        <div className="cell">
          <span className="field-error">{error}</span>
        </div>
      )}
    </Group>
  );
}

// ------------------------------------------------------------------ календарь (ICS)

export function CalendarFeedSection() {
  const api = useIntegrations();
  const { conn, serverUrl } = useSession();
  const { notify } = useData();
  const [feed, setFeed] = useState<CalendarFeed | null>(null);
  const [fresh, setFresh] = useState<string | null>(null);
  const [confirming, setConfirming] = useState<'reset' | 'disable' | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [copied, setCopied] = useState(false);

  const load = useCallback(async () => {
    try {
      setFeed(await api.calendarFeed());
      setError('');
    } catch (e) {
      if (!isNetworkError(e)) setError(failText(e));
    }
  }, [api]);

  useEffect(() => {
    if (conn === 'open') void load();
  }, [conn, load]);

  const reset = async () => {
    setBusy(true);
    setError('');
    try {
      const reply = await api.calendarFeedReset();
      setFeed(reply);
      const url = icsFeedUrl(reply.url, reply.path, serverUrl);
      setFresh(url);
      if (!url) setError('Ссылку не собрать: адрес сервера неизвестен. Задайте на сервере SELFVOY_SERVER_PUBLIC_URL.');
      setConfirming(null);
    } catch (e) {
      setError(failText(e));
    } finally {
      setBusy(false);
    }
  };

  const disable = async () => {
    setBusy(true);
    setError('');
    try {
      await api.calendarFeedDisable();
      setFeed({ enabled: false, url: '', path: '' });
      setFresh(null);
      setConfirming(null);
      notify('Подписка на календарь выключена');
    } catch (e) {
      setError(failText(e));
    } finally {
      setBusy(false);
    }
  };

  const webcal = webcalUrl(fresh);

  return (
    <Group head="Календарь в телефоне" foot="Встречи записей и принятые приглашения появятся в Google, Apple или Outlook Календаре — «Добавить календарь по URL». Календарь обновляется раз в час.">
      <div className="cell has-ico">
        <span className="ico accent">
          <CalendarDays size={16} />
        </span>
        <span className="lab">
          Подписка по ссылке
          <small>{feed === null ? 'Проверяем…' : feed.enabled ? 'Включена' : 'Выключена'}</small>
        </span>
        {feed === null ? (
          <Spinner small />
        ) : feed.enabled ? (
          <StatusPill status={{ label: 'Включена', tone: 'green', icon: 'check' }} />
        ) : (
          <button type="button" className="btn btn-primary btn-sm" disabled={busy} onClick={reset}>
            {busy ? <Spinner small /> : 'Включить'}
          </button>
        )}
      </div>
      {fresh && (
        <div className="cell stacked">
          <span className="small muted">Ваша ссылка — покажем её только сейчас. Не публикуйте: по ней видны ваши встречи.</span>
          <code className="linkbox">{fresh}</code>
          <div className="row wrap" style={{ gap: 8 }}>
            <button
              type="button"
              className="btn btn-primary btn-sm"
              onClick={async () => {
                const ok = await copyText(fresh);
                setCopied(ok);
                if (ok) setTimeout(() => setCopied(false), 2000);
              }}
            >
              <Copy size={14} /> {copied ? 'Скопировано' : 'Скопировать'}
            </button>
            {webcal && (
              <a className="btn btn-secondary btn-sm" href={webcal}>
                <CalendarDays size={14} /> Открыть в календаре
              </a>
            )}
          </div>
        </div>
      )}
      {feed?.enabled && !fresh && (
        <div className="cell stacked">
          <span className="small muted">Ссылка показывается один раз. Потеряли её или хотите отключить старую — создайте новую.</span>
        </div>
      )}
      {feed?.enabled && (
        <div className="cell stacked">
          {confirming === 'reset' ? (
            <div className="notice warn" style={{ flexWrap: 'wrap', alignItems: 'center' }}>
              <span style={{ flex: '1 1 220px' }}>Старая ссылка перестанет работать во всех календарях, где она добавлена. Создать новую?</span>
              <button type="button" className="btn btn-primary btn-sm" disabled={busy} onClick={reset}>
                {busy ? <Spinner small /> : 'Да, новая ссылка'}
              </button>
              <button type="button" className="btn btn-ghost btn-sm" onClick={() => setConfirming(null)}>
                Нет
              </button>
            </div>
          ) : confirming === 'disable' ? (
            <div className="notice error" style={{ flexWrap: 'wrap', alignItems: 'center' }}>
              <span style={{ flex: '1 1 220px' }}>Календари перестанут получать ваши встречи. Выключить подписку?</span>
              <button type="button" className="btn btn-danger btn-sm" disabled={busy} onClick={disable}>
                {busy ? <Spinner small /> : 'Выключить'}
              </button>
              <button type="button" className="btn btn-ghost btn-sm" onClick={() => setConfirming(null)}>
                Нет
              </button>
            </div>
          ) : (
            <div className="row wrap" style={{ gap: 8 }}>
              <button type="button" className="btn btn-secondary btn-sm" onClick={() => setConfirming('reset')}>
                <RefreshCw size={14} /> Новая ссылка
              </button>
              <button type="button" className="btn btn-ghost btn-sm" style={{ color: 'var(--danger)' }} onClick={() => setConfirming('disable')}>
                Выключить
              </button>
            </div>
          )}
        </div>
      )}
      {error && (
        <div className="cell">
          <span className="field-error">{error}</span>
        </div>
      )}
    </Group>
  );
}

// ------------------------------------------------------------------ ЮKassa

export function PaymentsSection() {
  const api = useIntegrations();
  const { conn } = useSession();
  const { notify } = useData();
  const [status, setStatus] = useState<PaymentsStatus | null>(null);
  const [shopId, setShopId] = useState('');
  const [secret, setSecret] = useState('');
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    try {
      setStatus(await api.paymentsStatus());
      setError('');
    } catch (e) {
      if (!isNetworkError(e)) setError(failText(e));
    }
  }, [api]);

  useEffect(() => {
    if (conn === 'open') void load();
  }, [conn, load]);

  // Уходим со страницы — ключ из памяти поля тоже уходит.
  useEffect(() => () => setSecret(''), []);

  const connect = async (e: FormEvent) => {
    e.preventDefault();
    const shop = shopId.trim();
    const key = secret.trim();
    if (!/^\d{1,20}$/.test(shop)) return setError('shopId — только цифры, до 20 знаков');
    if (!/^(live|test)_\S+$/.test(key)) return setError('Секретный ключ начинается с live_ или test_');
    setBusy(true);
    setError('');
    try {
      const reply = await api.paymentsConnect(shop, key);
      setSecret('');
      setStatus(reply);
      notify('ЮKassa подключена — гости смогут платить картой и через СБП');
    } catch (err) {
      setError(failText(err));
    } finally {
      setBusy(false);
    }
  };

  const disconnect = async () => {
    setBusy(true);
    try {
      await api.paymentsDisconnect();
      setConfirming(false);
      notify('ЮKassa отключена');
      await load();
    } catch (err) {
      setError(failText(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Group
      head={
        <span className="row" style={{ gap: 8 }}>
          Оплата: ЮKassa <ProBadge />
        </span>
      }
      foot="Свой магазин ЮKassa: деньги гостей идут прямо вам, Selfvoy их не касается. Ключ хранится на сервере зашифрованным и никому не показывается. Чеки — в «Мой налог» или в кабинете ЮKassa."
    >
      <div className="cell has-ico">
        <span className="ico amber">
          <CreditCard size={16} />
        </span>
        <span className="lab">
          ЮKassa
          <small>{status === null ? 'Проверяем…' : status.connected ? `Магазин ${status.shop_id}${status.connected_at ? ` · с ${when(status.connected_at)}` : ''}` : 'Не подключена'}</small>
        </span>
        {status === null ? (
          <Spinner small />
        ) : status.connected ? (
          <StatusPill status={{ label: 'Подключена', tone: 'green', icon: 'check' }} />
        ) : null}
      </div>
      {status?.connected && (
        <div className="cell">
          {confirming ? (
            <div className="row wrap" style={{ gap: 8, width: '100%' }}>
              <span className="small" style={{ marginRight: 'auto' }}>
                Новые записи с предоплатой перейдут на ссылку оплаты и реквизиты.
              </span>
              <button type="button" className="btn btn-danger btn-sm" disabled={busy} onClick={disconnect}>
                {busy ? <Spinner small /> : 'Отключить'}
              </button>
              <button type="button" className="btn btn-ghost btn-sm" onClick={() => setConfirming(false)}>
                Нет
              </button>
            </div>
          ) : (
            <button type="button" className="btn btn-ghost btn-sm" style={{ color: 'var(--danger)' }} onClick={() => setConfirming(true)}>
              Отключить ЮKassa
            </button>
          )}
        </div>
      )}
      {status && !status.connected && (
        <form className="cell stacked" onSubmit={connect} noValidate autoComplete="off">
          <span className="small muted">Ключи — в личном кабинете ЮKassa: «Интеграция» → «Ключи API».</span>
          <input className="input" inputMode="numeric" placeholder="shopId, например 123456" aria-label="shopId магазина" value={shopId} maxLength={20} onChange={(e) => setShopId(e.target.value.replace(/\D/g, ''))} />
          <PasswordInput
            placeholder="Секретный ключ live_… или test_…"
            aria-label="Секретный ключ"
            autoComplete="off"
            spellCheck={false}
            data-lpignore="true"
            value={secret}
            onChange={(e) => setSecret(e.target.value)}
          />
          <button type="submit" className="btn btn-primary" disabled={busy || !shopId || !secret}>
            {busy ? <Spinner small /> : 'Подключить'}
          </button>
        </form>
      )}
      {error && (
        <div className="cell">
          <span className="field-error">{error}</span>
        </div>
      )}
    </Group>
  );
}
