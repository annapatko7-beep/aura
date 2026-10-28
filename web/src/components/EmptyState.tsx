import type { ComponentType } from 'react';

/**
  Красивое пустое состояние: иконка в цветном квадрате, заголовок, описание и
  опциональная кнопка действия. Заменяет текстовые <Empty> там, где пользователь
  впервые видит раздел.
*/
export function EmptyState({ icon: Icon, title, desc, action, onAction }: {
  icon: ComponentType<{ size?: number; strokeWidth?: number }>;
  title: string;
  desc: string;
  action?: string;
  onAction?: () => void;
}) {
  return (
    <div className="empty-state">
      <span className="empty-icon" aria-hidden="true">
        <Icon size={28} strokeWidth={1.5} />
      </span>
      <h3>{title}</h3>
      <p>{desc}</p>
      {action && onAction && (
        <button className="btn btn-primary btn-sm" onClick={onAction} type="button">
          {action}
        </button>
      )}
    </div>
  );
}
