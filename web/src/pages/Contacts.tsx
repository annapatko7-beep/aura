import { ArrowLeft, Handshake, Search, UserPlus, Users } from 'lucide-react';
import { useCallback, useEffect, useRef, useState } from 'react';
import { Link, useNavigate, useParams } from 'react-router-dom';
import { CommandBar } from '../components/CommandBar';
import { Avatar, Empty, Spinner } from '../components/ui';
import { shortWhen } from '../lib/format';
import type { FoundUser, Message } from '../net/api';
import { useData } from '../state/data';
import { errorText, useSession } from '../state/session';
import { SkeletonList } from '../components/Skeleton';
import { EmptyState } from '../components/EmptyState';

export function Contacts() {
  const { api, user } = useSession();
  const { contacts, openChat, loaded } = useData();
  const navigate = useNavigate();
  const [query, setQuery] = useState('');
  const [found, setFound] = useState<FoundUser[]>([]);
  const [searching, setSearching] = useState(false);

  useEffect(() => {
    const q = query.trim();
    if (q.length < 2) {
      setFound([]);
      return;
    }
    setSearching(true);
    // Ответ на устаревший запрос (пользователь печатает дальше) игнорируется:
    // иначе медленный ответ на «ал» перетёр бы результаты для «алекс».
    let current = true;
    const t = setTimeout(() => {
      api
        .searchUsers(q)
        .then((r) => {
          if (current) setFound((r.users ?? []).filter((u) => u.id !== user?.id && !contacts.some((c) => c.userId === u.id)));
        })
        .catch(() => {
          if (current) setFound([]);
        })
        .finally(() => {
          if (current) setSearching(false);
        });
    }, 300);
    return () => {
      current = false;
      clearTimeout(t);
      setSearching(false);
    };
  }, [query, api, user?.id, contacts]);

  const visible = contacts.filter((c) => {
    const q = query.trim().toLowerCase();
    return !q || c.name.toLowerCase().includes(q) || c.email.toLowerCase().includes(q);
  });

  const add = async (u: FoundUser) => {
    const chatId = await openChat(u.email);
    if (chatId) navigate(`/contacts/${chatId}`);
  };

  return (
    <div className="page">
      <header className="page-head">
        <div>
          <h1 className="title-xl">Контакты</h1>
          <div className="sub">Люди, с чьими Selfvoy может договариваться ваш Selfvoy.</div>
        </div>
      </header>
      <div className="stack-lg">
        <label className="command" style={{ height: 46 }}>
          <Search size={18} className="faint" />
          <input value={query} onChange={(e) => setQuery(e.target.value)} placeholder="Найти по имени или email" aria-label="Поиск людей" />
          {searching && <Spinner small />}
        </label>

        <div className="card flush list">
          {!loaded && <SkeletonList rows={4} />}
          {loaded && visible.length === 0 && !found.length && <EmptyState icon={Users} title="Нет контактов" desc="Контакты появятся, когда вы начнёте переговоры или получите приглашение." />}
          {visible.map((c) => (
            <Link key={c.userId} to={`/contacts/${c.chatId}`} className="list-row">
              <Avatar name={c.name} src={c.avatar} size={40} />
              <div className="body">
                <div className="ellipsis" style={{ fontWeight: 600 }}>
                  {c.name}
                </div>
                <div className="small muted ellipsis">{c.lastBody || c.email}</div>
              </div>
              {c.lastAt && <span className="meta">{shortWhen(c.lastAt)}</span>}
            </Link>
          ))}
        </div>

        {found.length > 0 && (
          <section>
            <h2 className="title-sm section-title">Найдено в Selfvoy</h2>
            <div className="card flush list">
              {found.map((u) => (
                <div key={u.id} className="list-row">
                  <Avatar name={u.display_name || u.email} src={u.avatar_url} size={40} />
                  <div className="body">
                    <div className="ellipsis" style={{ fontWeight: 600 }}>{u.display_name}</div>
                    <div className="small muted ellipsis">{u.email}</div>
                  </div>
                  <button className="btn btn-secondary btn-sm" onClick={() => add(u)}>
                    <UserPlus size={16} /> Написать
                  </button>
                </div>
              ))}
            </div>
          </section>
        )}
      </div>
    </div>
  );
}

export function ContactChat() {
  const { chatId } = useParams();
  const id = Number(chatId);
  const { api, user } = useSession();
  const { contacts, chatPulse, notify } = useData();
  const navigate = useNavigate();
  const contact = contacts.find((c) => c.chatId === id);
  // История помечена id чата: при переходе между чатами старая не показывается
  // ни секунды, а запоздалый ответ по прошлому чату отбрасывается.
  const [history, setHistory] = useState<{ chatId: number; messages: Message[] } | null>(null);
  const messages = history && history.chatId === id ? history.messages : null;
  const currentId = useRef(id);
  currentId.current = id;
  const seq = useRef(0);
  const end = useRef<HTMLDivElement>(null);

  const load = useCallback(() => {
    const ticket = ++seq.current;
    const fresh = () => currentId.current === id && seq.current === ticket;
    api
      .history(id)
      .then((r) => {
        if (fresh()) setHistory({ chatId: id, messages: [...(r.messages ?? [])].sort((a, b) => a.id - b.id) });
      })
      .catch(() => {
        if (fresh()) setHistory((h) => (h && h.chatId === id ? h : { chatId: id, messages: [] }));
      });
  }, [api, id]);

  useEffect(load, [load, chatPulse]);
  useEffect(() => {
    end.current?.scrollIntoView({ block: 'end' });
  }, [messages?.length]);

  const send = useCallback(
    async (text: string) => {
      try {
        await api.send(id, text);
        load();
      } catch (error) {
        notify(errorText(error), 'error');
      }
    },
    [api, id, load, notify],
  );

  return (
    <div className="chat">
      <header className="chat-head">
        <Link to="/contacts" className="icon-btn" aria-label="Назад">
          <ArrowLeft size={18} />
        </Link>
        <Avatar name={contact?.name ?? 'Чат'} src={contact?.avatar} size={34} />
        <div style={{ minWidth: 0, flex: 1 }}>
          <div className="title-md ellipsis">{contact?.name ?? 'Переписка'}</div>
          {contact && <div className="tiny muted ellipsis">{contact.email}</div>}
        </div>
        {contact && (
          <button className="btn btn-secondary btn-sm" onClick={() => navigate(`/negotiations?new=1&to=${encodeURIComponent(contact.email)}`)}>
            <Handshake size={14} /> Договориться
          </button>
        )}
      </header>
      <div className="messages">
        <div className="messages-inner">
          {messages === null && <SkeletonList rows={3} />}
          {messages?.length === 0 && <Empty>Сообщений пока нет — напишите первым.</Empty>}
          {messages?.map((m) => {
            const mine = m.sender_id === user?.id;
            const agent = m.kind !== 'text';
            return (
              <div key={m.id} style={{ display: 'contents' }}>
                <div className={`msg ${mine ? 'mine' : 'theirs'}`}>{m.body}</div>
                <div className={`msg-meta ${mine ? 'mine' : ''}`}>
                  {agent ? 'Selfvoy · ' : ''}
                  {shortWhen(m.created_at)}
                </div>
              </div>
            );
          })}
          <div ref={end} />
        </div>
      </div>
      <div className="composer">
        <CommandBar onSubmit={send} placeholder="Сообщение…" />
      </div>
    </div>
  );
}
