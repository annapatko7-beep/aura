import { ArrowRight, Check, X } from 'lucide-react';
import { useCallback, useEffect, useRef } from 'react';
import { Link } from 'react-router-dom';
import { CommandBar } from '../components/CommandBar';
import { Logo } from '../components/ui';
import { firstName, toolTitle } from '../lib/format';
import { useData, type DialogEntry } from '../state/data';
import { useSession } from '../state/session';

const SUGGESTIONS = ['Хочу встретиться с Димой в выходные и обсудить стартап', 'Что у меня сегодня?', 'Напомни завтра в 10 позвонить юристу'];

export function Chat() {
  const { user } = useSession();
  const { dialog, thinking, ask } = useData();
  const end = useRef<HTMLDivElement>(null);

  useEffect(() => {
    end.current?.scrollIntoView({ block: 'end' });
  }, [dialog.length, thinking]);

  const send = useCallback((text: string) => void ask(text), [ask]);

  return (
    <div className="chat">
      <header className="chat-head">
        <span className="logo-tile" style={{ width: 36, height: 36, borderRadius: 10 }}>
          <Logo size={18} />
        </span>
        <div style={{ minWidth: 0 }}>
          <div className="title-md">Selfvoy</div>
          <div className="tiny muted">{thinking ? 'Думает…' : 'Персональный ассистент'}</div>
        </div>
      </header>

      <div className="messages">
        <div className="messages-inner" role="log" aria-live="polite" aria-label="Переписка с Selfvoy">
          {dialog.length === 0 && (
            <div className="stack" style={{ marginTop: 24 }}>
              <div className="msg theirs">
                Привет, {firstName(user?.display_name ?? '')}! Скажите, что нужно сделать, — я договорюсь с Selfvoy других людей, поставлю напоминание или подготовлю письмо. Опасные действия выполняю только после вашего подтверждения.
              </div>
              <div className="suggestions">
                {SUGGESTIONS.map((s) => (
                  <button key={s} className="chip" onClick={() => send(s)}>
                    {s}
                  </button>
                ))}
              </div>
            </div>
          )}
          {dialog.map((m) => (
            <Message key={m.id} entry={m} />
          ))}
          {thinking && (
            <div className="msg-row theirs">
              <span className="msg-avatar thinking-pulse"><Logo size={16} mono /></span>
              <div className="msg thinking-msg">
                <span className="typing-text">Думает</span>
                <span className="typing" aria-label="Selfvoy думает"><i /><i /><i /></span>
              </div>
            </div>
          )}
          {!thinking && dialog.length > 0 && dialog.at(-1)?.from === 'selfvoy' && (
            <div className="suggestions follow-up">
              {['Подробнее', 'Что ещё?', 'Спасибо'].map((s) => (
                <button key={s} className="chip" onClick={() => send(s)}>{s}</button>
              ))}
            </div>
          )}
          <div ref={end} />
        </div>
      </div>

      <div className="composer">
        <CommandBar onSubmit={send} busy={thinking} placeholder="Напишите Selfvoy…" />
      </div>
    </div>
  );
}

function Message({ entry }: { entry: DialogEntry }) {
  const { confirmations, approve, deny } = useData();
  const mine = entry.from === 'user';
  const pending = new Set(confirmations.map((c) => c.id));
  return (
    <div className={`msg-row ${mine ? 'mine' : 'theirs'}`}>
      {!mine && <span className="msg-avatar"><Logo size={16} mono /></span>}
      <div className={`msg ${entry.error ? 'error' : ''}`}>
      {entry.text}
      {(entry.actions?.length || entry.negotiationId) && (
        <div className="actions">
          {entry.actions?.map((a) => {
            const open = pending.has(a.confirmationId);
            return (
              <div key={a.confirmationId} className="stack" style={{ gap: 8 }}>
                <div className="small">
                  <span className={open ? '' : 'faint'}>{open ? 'Ждёт вашего решения' : 'Решено'}</span> · {toolTitle(a.tool)}: {a.summary}
                </div>
                {open && (
                  <div className="row">
                    <button className="btn btn-primary btn-sm" onClick={() => approve(a.confirmationId)}>
                      <Check size={14} /> Подтвердить
                    </button>
                    <button className="btn btn-ghost btn-sm" onClick={() => deny(a.confirmationId)}>
                      <X size={14} /> Отклонить
                    </button>
                  </div>
                )}
              </div>
            );
          })}
          {entry.negotiationId && (
            <Link className="link small" to={`/negotiations/${entry.negotiationId}`}>
              Открыть переговоры <ArrowRight size={13} style={{ verticalAlign: -2 }} />
            </Link>
          )}
        </div>
      )}
      </div>
    </div>
  );
}
