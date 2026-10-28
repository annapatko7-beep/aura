import { Bell, Check, CheckCheck, ChevronRight, Package, ShieldAlert, X } from 'lucide-react';
import { Link } from 'react-router-dom';
import { Empty } from '../components/ui';
import { EmptyState } from '../components/EmptyState';
import { toolTitle, when } from '../lib/format';
import { isPackageNotice, packageNoticeDetail } from '../lib/packages';
import type { Notice } from '../net/api';
import { useData } from '../state/data';

/** Текст уведомления без технических полей: для «нового входа» — устройство, а не сетевой адрес. */
function detail(n: Notice): string {
  if (n.kind === 'login.new') {
    const device = (n.payload as { device?: string } | undefined)?.device;
    return device ? `Устройство: ${device}` : '';
  }
  // Абонементы (F5): сервер присылает текст; старый или пустой — собираем из payload.
  if (isPackageNotice(n.kind)) return n.body || packageNoticeDetail(n.kind, n.payload);
  return n.body;
}

export function Inbox() {
  const { confirmations, notices, unread, approve, deny, readAll } = useData();
  return (
    <div className="page">
      <header className="page-head">
        <div>
          <h1 className="title-xl">Входящие</h1>
          <div className="sub">Решения, которых ждёт Selfvoy, и уведомления.</div>
        </div>
        {unread > 0 && (
          <button className="btn btn-ghost" onClick={readAll}>
            <CheckCheck size={16} /> Прочитать все
          </button>
        )}
      </header>
      <div className="stack-lg">
        <section>
          <h2 className="title-sm section-title">Ждут решения · {confirmations.length}</h2>
          <div className="card flush list">
            {confirmations.length === 0 && <Empty>Selfvoy ничего не ждёт. Опасные действия — письма, сообщения, бронь — появятся здесь до выполнения.</Empty>}
            {confirmations.map((c) => (
              <div key={c.id} className="list-row" style={{ flexWrap: 'wrap' }}>
                <ShieldAlert size={20} style={{ color: 'var(--warning)', flex: 'none' }} />
                <div className="body">
                  <div style={{ fontWeight: 600 }}>{c.summary || toolTitle(c.tool)}</div>
                  <div className="small muted">
                    {toolTitle(c.tool)}
                    {c.created_at ? ` · ${when(c.created_at)}` : ''}
                  </div>
                </div>
                <div className="row">
                  <button className="btn btn-primary btn-sm" onClick={() => approve(c.id)}>
                    <Check size={16} /> Подтвердить
                  </button>
                  <button className="btn btn-ghost btn-sm" onClick={() => deny(c.id)}>
                    <X size={16} /> Отклонить
                  </button>
                </div>
              </div>
            ))}
          </div>
        </section>

        <section>
          <h2 className="title-sm section-title">Уведомления</h2>
          <div className="card flush list">
            {notices.length === 0 && <EmptyState icon={Bell} title="Всё прочитано" desc="Уведомления и запросы на подтверждение появятся здесь." />}
            {notices.map((n) => {
              const pkg = isPackageNotice(n.kind);
              const content = (
                <>
                  {!n.read ? <span className="dot accent" style={{ margin: '0 4px', width: 10, height: 10 }} /> : pkg ? <Package size={18} className="faint" /> : <Bell size={18} className="faint" />}
                  <div className="body">
                    <div style={{ fontWeight: n.read ? 450 : 650 }}>{n.title}</div>
                    {detail(n) && <div className="small muted">{detail(n)}</div>}
                  </div>
                  <span className="meta">{when(n.created_at)}</span>
                  {pkg && <ChevronRight size={18} className="chev" aria-hidden="true" />}
                </>
              );
              const id = Number((n.payload as { package_id?: unknown } | undefined)?.package_id ?? 0);
              return pkg ? (
                <Link key={n.id} to={id > 0 ? `/packages?id=${id}` : '/packages'} className="list-row">
                  {content}
                </Link>
              ) : (
                <div key={n.id} className="list-row">
                  {content}
                </div>
              );
            })}
          </div>
        </section>
      </div>
    </div>
  );
}
