import { Check, CheckSquare, Plus, Trash2 } from 'lucide-react';
import { useState, type FormEvent } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { Spinner } from '../components/ui';
import { SkeletonList } from '../components/Skeleton';
import { EmptyState } from '../components/EmptyState';
import { parseDate, shortWhen } from '../lib/format';
import { sameDay, startOfDay } from '../lib/time';
import type { Task } from '../net/api';
import { useData } from '../state/data';

function due(t: Task): Date | null {
  return parseDate(t.due_at || t.remind_at);
}

export function Tasks() {
  const { tasks, loaded, addTask, toggleTask, deleteTask } = useData();
  const navigate = useNavigate();
  const [title, setTitle] = useState('');
  const [at, setAt] = useState('');
  const [busy, setBusy] = useState(false);
  const [showDone, setShowDone] = useState(false);
  // «+» → «Новая задача» открывает страницу с курсором в поле.
  const [params] = useSearchParams();
  const focusNew = params.get('new') === '1';

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    if (!title.trim()) return;
    setBusy(true);
    const ok = await addTask(title, at ? new Date(at) : undefined);
    setBusy(false);
    if (ok) {
      setTitle('');
      setAt('');
    }
  };

  const now = new Date();
  const today = startOfDay(now);
  const open = tasks.filter((t) => t.status === 'pending').sort((a, b) => (due(a)?.getTime() ?? Infinity) - (due(b)?.getTime() ?? Infinity));
  const groups: { title: string; items: Task[] }[] = [
    { title: 'Просрочено', items: open.filter((t) => due(t) && due(t)! < today) },
    { title: 'Сегодня', items: open.filter((t) => due(t) && sameDay(due(t)!, now)) },
    { title: 'Дальше', items: open.filter((t) => due(t) && due(t)! > now && !sameDay(due(t)!, now)) },
    { title: 'Без срока', items: open.filter((t) => !due(t)) },
  ].filter((g) => g.items.length);
  const done = tasks.filter((t) => t.status === 'done');

  return (
    <div className="page">
      <header className="page-head">
        <div>
          <h1 className="title-xl">Задачи</h1>
          <div className="sub">Напоминания, встречи из переговоров и всё, что поручили Selfvoy.</div>
        </div>
      </header>

      <div className="stack-lg">
        <form className="card row wrap" onSubmit={submit} style={{ padding: 10, gap: 8 }}>
          <input className="input" style={{ flex: '1 1 220px', border: 0, background: 'transparent', boxShadow: 'none' }} value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Новая задача" aria-label="Новая задача" maxLength={200} autoFocus={focusNew} />
          <input className="input" type="datetime-local" style={{ width: 'auto', flex: '0 1 220px' }} value={at} onChange={(e) => setAt(e.target.value)} aria-label="Срок" />
          <button className="btn btn-primary" type="submit" disabled={!title.trim() || busy}>
            {busy ? <Spinner small /> : <Plus size={16} />} Добавить
          </button>
        </form>

        {loaded && open.length === 0 && <EmptyState icon={CheckSquare} title="Задач пока нет" desc="Скажите ассистенту — он создаст напоминание или задачу со сроком." action="Спросить ассистента" onAction={() => navigate('/selfvoy')} />}
        {!loaded && <SkeletonList />}

        {groups.map((g) => (
          <section key={g.title}>
            <h2 className="title-sm section-title" style={g.title === 'Просрочено' ? { color: 'var(--danger)' } : undefined}>
              {g.title} · {g.items.length}
            </h2>
            <div className="card flush list">
              {g.items.map((t) => (
                <TaskRow key={t.id} task={t} onToggle={toggleTask} onDelete={deleteTask} />
              ))}
            </div>
          </section>
        ))}

        {done.length > 0 && (
          <section>
            <button className="title-sm section-title" onClick={() => setShowDone((v) => !v)}>
              Выполнено · {done.length} {showDone ? '▾' : '▸'}
            </button>
            {showDone && (
              <div className="card flush list">
                {done.map((t) => (
                  <TaskRow key={t.id} task={t} onToggle={toggleTask} onDelete={deleteTask} />
                ))}
              </div>
            )}
          </section>
        )}
      </div>
    </div>
  );
}

function TaskRow({ task, onToggle, onDelete }: { task: Task; onToggle: (t: Task) => void; onDelete: (id: number) => void }) {
  const isDone = task.status === 'done';
  return (
    <div className="list-row">
      <button className={`check ${isDone ? 'on' : ''}`} onClick={() => onToggle(task)} aria-label={isDone ? `Вернуть: ${task.title}` : `Выполнено: ${task.title}`}>
        <Check size={15} strokeWidth={3} />
      </button>
      <div className="body">
        <div className={`task-title ${isDone ? 'done' : ''}`}>{task.title}</div>
        {task.notes && <div className="small muted ellipsis">{task.notes}</div>}
      </div>
      <span className="meta">{shortWhen(task.due_at || task.remind_at)}</span>
      <button className="icon-btn hover-only" onClick={() => onDelete(task.id)} aria-label={`Удалить: ${task.title}`}>
        <Trash2 size={17} />
      </button>
    </div>
  );
}
