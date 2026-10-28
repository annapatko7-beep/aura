/*
  «Ещё» — четвёртая вкладка на телефоне: профиль и тариф, остальные разделы
  приложения и настройки. На десктопе то же самое есть в боковой панели.
*/
import { Bell, CalendarDays, CheckSquare, ChevronRight, CreditCard, Gift, Handshake, Package, Palette, Settings, Shield, Users } from 'lucide-react';
import { Link } from 'react-router-dom';
import { Avatar, Group, ProBadge } from '../components/ui';
import { planView } from '../lib/integrations';
import { isStaff } from '../net/adminApi';
import { useData } from '../state/data';
import { useSession } from '../state/session';

export function More() {
  const { user } = useSession();
  const { unread, confirmations } = useData();
  const inbox = unread + confirmations.length;
  const plan = planView(user?.plan);

  const cell = (to: string, label: string, Icon: typeof Bell, tone: string, value?: string) => (
    <Link to={to} className="cell has-ico">
      <span className={`ico ${tone}`}>
        <Icon size={19} />
      </span>
      <span className="lab">{label}</span>
      {value && <span className="val">{value}</span>}
      <ChevronRight size={18} className="chev" aria-hidden="true" />
    </Link>
  );

  return (
    <div className="page narrow">
      <header className="page-head">
        <h1 className="title-xl">Ещё</h1>
      </header>
      <div className="stack-lg">
        {user && (
          <div className="group-body">
            <Link to="/settings" className="cell">
              <Avatar name={user.display_name || user.email} src={user.avatar_url} size={52} />
              <span className="lab">
                <b style={{ fontSize: 18 }}>{user.display_name || user.email}</b>
                <small>{plan ? `${plan.label} · ${plan.hint}` : user.email}</small>
              </span>
              {plan?.pro && <ProBadge />}
              <ChevronRight size={18} className="chev" aria-hidden="true" />
            </Link>
          </div>
        )}
        <Group>
          {cell('/inbox', 'Входящие', Bell, 'accent', inbox > 0 ? String(inbox) : undefined)}
          {cell('/packages', 'Абонементы', Package, 'amber')}
          {cell('/negotiations', 'Переговоры', Handshake, 'green')}
          {cell('/tasks', 'Задачи', CheckSquare, 'violet')}
          {cell('/contacts', 'Контакты', Users, 'pink')}
        </Group>
        <Group>
          {cell('/settings?s=appearance', 'Оформление и темы', Palette, 'violet')}
          {cell('/settings?s=integrations', 'Telegram и календарь', CalendarDays, 'ink')}
          {cell('/settings?s=integrations', 'Оплата: ЮKassa', CreditCard, 'amber')}
          {cell('/settings?s=referral', 'Месяц Pro за коллегу', Gift, 'accent')}
          {cell('/settings', 'Настройки', Settings, '')}
          {isStaff(user?.role) && cell('/admin', 'Администрирование', Shield, '')}
        </Group>
      </div>
    </div>
  );
}
