/*
  Оболочка приложения. На телефоне — как нативное приложение: плавающая стеклянная
  панель вкладок (Сегодня, Запись, ИИ, Ещё) и отдельная стеклянная кнопка «+»
  с быстрыми действиями. На десктопе — боковая панель с теми же разделами.
*/
import {
  Bell,
  CalendarClock,
  CalendarDays,
  CheckSquare,
  Ellipsis,
  Handshake,
  Megaphone,
  Package,
  Plus,
  Settings,
  Shield,
  Sparkles,
  Users,
} from 'lucide-react';
import { useState } from 'react';
import { NavLink, Outlet, useNavigate } from 'react-router-dom';
import { planView } from '../lib/integrations';
import { isStaff } from '../net/adminApi';
import { useData } from '../state/data';
import { useSession } from '../state/session';
import { OfflineBanner } from './ConnectionScreen';
import { Avatar, Logo, ProBadge, Sheet } from './ui';

const TABS = [
  { to: '/', label: 'Сегодня', icon: CalendarDays, end: true },
  { to: '/booking', label: 'Запись', icon: CalendarClock },
  { to: '/selfvoy', label: 'ИИ', icon: Sparkles },
  { to: '/more', label: 'Ещё', icon: Ellipsis },
];

const SIDE = [
  { to: '/', label: 'Сегодня', icon: CalendarDays, end: true },
  { to: '/booking', label: 'Запись', icon: CalendarClock },
  { to: '/packages', label: 'Абонементы', icon: Package },
  { to: '/selfvoy', label: 'Ассистент', icon: Sparkles },
  { to: '/negotiations', label: 'Переговоры', icon: Handshake },
  { to: '/tasks', label: 'Задачи', icon: CheckSquare },
  { to: '/contacts', label: 'Контакты', icon: Users },
];

/** Быстрые действия кнопки «+». */
const QUICK = [
  { to: '/booking?new=1', label: 'Новая страница записи', hint: 'Ссылка, по которой клиенты записываются сами', icon: CalendarClock, tone: 'accent' },
  { to: '/packages?new=1', label: 'Выдать абонемент', hint: 'Пакет занятий клиенту — со ссылкой на остаток', icon: Package, tone: 'pink' },
  { to: '/booking?post=1', label: 'Пост со свободными окнами', hint: 'Готовый текст для Telegram или VK', icon: Megaphone, tone: 'amber' },
  { to: '/negotiations?new=1', label: 'Договориться о встрече', hint: 'Selfvoy согласует время с другим Selfvoy', icon: Handshake, tone: 'green' },
  { to: '/tasks?new=1', label: 'Новая задача', hint: 'Напоминание или дело со сроком', icon: CheckSquare, tone: 'violet' },
  { to: '/selfvoy', label: 'Спросить ассистента', hint: 'Текстом или голосом', icon: Sparkles, tone: 'ink' },
] as const;

export function AppShell() {
  const { user } = useSession();
  const { unread, confirmations, toast } = useData();
  const navigate = useNavigate();
  const [quick, setQuick] = useState(false);
  const inbox = unread + confirmations.length;
  const plan = planView(user?.plan);

  return (
    <div className="app">
      <aside className="sidebar" aria-label="Навигация">
        <div className="brand">
          <Logo size={26} /> Selfvoy
        </div>
        <button type="button" className="btn btn-primary sidebar-new" onClick={() => setQuick(true)}>
          <Plus size={17} /> Создать
        </button>
        {SIDE.map(({ to, label, icon: Icon, end }) => (
          <NavLink key={to} to={to} end={end} className="nav-item">
            <Icon size={18} strokeWidth={1.9} /> {label}
          </NavLink>
        ))}
        <NavLink to="/inbox" className="nav-item">
          <Bell size={18} strokeWidth={1.9} /> Входящие
          {inbox > 0 && <span className="count">{inbox}</span>}
        </NavLink>
        <div className="sidebar-foot">
          {isStaff(user?.role) && (
            <NavLink to="/admin" className="nav-item">
              <Shield size={18} strokeWidth={1.9} /> Администрирование
            </NavLink>
          )}
          <NavLink to="/settings" className="nav-item">
            <Settings size={18} strokeWidth={1.9} /> Настройки
          </NavLink>
          {user && (
            <NavLink to="/settings" className="me-row">
              <Avatar name={user.display_name || user.email} src={user.avatar_url} size={32} />
              <span style={{ minWidth: 0, flex: 1 }}>
                <span className="small ellipsis" style={{ display: 'block', fontWeight: 600 }}>
                  {user.display_name}
                </span>
                <span className="tiny muted ellipsis" style={{ display: 'block' }}>
                  {plan ? plan.label : user.email}
                </span>
              </span>
              {plan?.pro && <ProBadge />}
            </NavLink>
          )}
        </div>
      </aside>

      <main className="main">
        <OfflineBanner />
        <Outlet />
      </main>

      <nav className="tab-bar" aria-label="Вкладки">
        {TABS.map(({ to, label, icon: Icon, end }) => (
          <NavLink key={to} to={to} end={end}>
            <Icon size={24} strokeWidth={1.9} />
            {label}
            {to === '/more' && inbox > 0 && <span className="count" aria-label={`Входящих: ${inbox}`} />}
          </NavLink>
        ))}
      </nav>
      <button type="button" className="fab glass" aria-label="Создать" onClick={() => setQuick(true)}>
        <Plus size={26} strokeWidth={2.2} />
      </button>

      <Sheet open={quick} onClose={() => setQuick(false)} title="Создать">
        <div className="group-body">
          {QUICK.map(({ to, label, hint, icon: Icon, tone }) => (
            <button
              key={to}
              type="button"
              className="cell has-ico"
              onClick={() => {
                setQuick(false);
                navigate(to);
              }}
            >
              <span className={`ico ${tone}`}>
                <Icon size={19} />
              </span>
              <span className="lab">
                {label}
                <small>{hint}</small>
              </span>
            </button>
          ))}
        </div>
      </Sheet>

      {toast && (
        <div key={toast.id} className={`toast ${toast.kind === 'error' ? 'error' : ''}`} role="status">
          {toast.text}
        </div>
      )}
    </div>
  );
}
