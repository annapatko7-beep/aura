import type { CSSProperties } from 'react';

/**
  Скелетоны загрузки: анимированные плейсхолдеры, пока данные не пришли.
  Используют CSS-класс .skeleton с shimmer-анимацией из global.css.
*/

export function Skeleton({ width, height = 16, radius = 8, style, className = '' }: {
  width?: number | string; height?: number | string; radius?: number; style?: CSSProperties; className?: string;
}) {
  return (
    <span
      className={`skeleton ${className}`}
      style={{ width, height, borderRadius: radius, ...style }}
      aria-hidden="true"
    />
  );
}

/** Скелетон карточки: заголовок + N строк текста. */
export function SkeletonCard({ lines = 3 }: { lines?: number }) {
  return (
    <div className="card skeleton-card" aria-busy="true" aria-label="Загрузка">
      <Skeleton width="40%" height={20} radius={10} />
      {Array.from({ length: lines }, (_, i) => (
        <Skeleton
          key={i}
          width={i === lines - 1 ? '60%' : '100%'}
          style={{ marginTop: 10 }}
        />
      ))}
    </div>
  );
}

/** Скелетон списка: N строк с аватаром и двумя строками текста. */
export function SkeletonList({ rows = 4 }: { rows?: number }) {
  return (
    <div className="card flush list" aria-busy="true" aria-label="Загрузка">
      {Array.from({ length: rows }, (_, i) => (
        <div key={i} className="list-row">
          <Skeleton width={38} height={38} radius={19} />
          <div className="body" style={{ display: 'grid', gap: 6 }}>
            <Skeleton width="55%" height={14} radius={7} />
            <Skeleton width="35%" height={12} radius={6} />
          </div>
        </div>
      ))}
    </div>
  );
}

/** Скелетон профиля: аватар + имя + email. */
export function SkeletonProfile() {
  return (
    <div className="card stack" aria-busy="true" aria-label="Загрузка профиля">
      <Skeleton width="30%" height={20} radius={10} />
      <div className="row" style={{ gap: 16 }}>
        <Skeleton width={64} height={64} radius={32} />
        <div style={{ display: 'grid', gap: 8, flex: 1 }}>
          <Skeleton width="50%" height={14} radius={7} />
          <Skeleton width="70%" height={12} radius={6} />
        </div>
      </div>
      <Skeleton width="100%" height={46} radius={14} />
      <Skeleton width="100%" height={46} radius={14} />
      <Skeleton width="100%" height={46} radius={14} />
    </div>
  );
}

/** Скелетон карточки «Следующая встреча» на странице «Сегодня». */
export function SkeletonNextCard() {
  return (
    <div className="next skeleton-next" aria-busy="true" aria-label="Загрузка">
      <Skeleton width="40%" height={13} radius={7} />
      <Skeleton width="30%" height={36} radius={10} style={{ marginTop: 8 }} />
      <Skeleton width="55%" height={14} radius={7} style={{ marginTop: 8 }} />
      <div className="row" style={{ marginTop: 12, gap: 8 }}>
        <Skeleton width={100} height={36} radius={18} />
        <Skeleton width={80} height={36} radius={18} />
      </div>
    </div>
  );
}
