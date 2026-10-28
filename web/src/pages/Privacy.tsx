import { ArrowLeft, Shield } from 'lucide-react';
import { useNavigate } from 'react-router-dom';

export function Privacy() {
  const navigate = useNavigate();

  return (
    <div className="page narrow" style={{ padding: '24px 16px', maxWidth: 760, margin: '0 auto' }}>
      <header style={{ marginBottom: 24, display: 'flex', alignItems: 'center', gap: 12 }}>
        <button
          type="button"
          onClick={() => navigate(-1)}
          className="btn-icon"
          aria-label="Назад"
          style={{
            background: 'var(--bg-card, #fff)',
            border: '1px solid var(--border-color, #e2e8f0)',
            borderRadius: '50%',
            width: 38,
            height: 38,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            cursor: 'pointer',
          }}
        >
          <ArrowLeft size={18} />
        </button>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <Shield size={22} style={{ color: 'var(--accent, #6366f1)' }} />
          <h1 className="title-xl" style={{ margin: 0, fontSize: 24 }}>Политика конфиденциальности</h1>
        </div>
      </header>

      <div className="group-body stack-md" style={{ padding: 24, lineHeight: 1.6, fontSize: 15 }}>
        <p><em>Дата последнего обновления: 20 октября 2026 г.</em></p>

        <section>
          <h2>1. Общие положения</h2>
          <p>
            Настоящая Политика конфиденциальности определяет порядок обработки и защиты персональной информации
            пользователей приложения и сервиса <strong>Selfvoy</strong> (далее — «Сервис»).
            Используя Сервис, вы выражаете согласие на сбор и использование информации в соответствии с настоящей Политикой.
          </p>
        </section>

        <section>
          <h2>2. Какие данные мы собираем</h2>
          <p>Мы собираем только те данные, которые необходимы для функционирования учетной записи и возможностей Сервиса:</p>
          <ul>
            <li><strong>Учетные данные:</strong> адрес электронной почты (email), отображаемое имя (никнейм) и хэш пароля при стандартной регистрации.</li>
            <li><strong>Данные внешних профилей (OAuth 2.0):</strong> при быстрой регистрации через Google, GitHub, Яндекс, VK ID или Discord мы получаем исключительно идентификатор пользователя, адрес электронной почты, имя профиля и аватар.</li>
            <li><strong>Пользовательский контент:</strong> сообщения в чатах, создаваемые задачи, заметки и настройки ИИ-агентов.</li>
            <li><strong>Технические данные:</strong> IP-адрес для защиты от брутфорса, тип операционной системы, идентификаторы сессий и пуш-токены для доставки уведомлений.</li>
          </ul>
        </section>

        <section>
          <h2>3. Цели обработки информации</h2>
          <ul>
            <li>Аутентификация и авторизация пользователя в Сервисе.</li>
            <li>Обеспечение взаимодействия между ИИ-ассистентами и пользователем.</li>
            <li>Отправка служебных уведомлений (подтверждение почты, сброс пароля, важные оповещения).</li>
            <li>Предотвращение мошенничества, спама и вредоносных действий.</li>
          </ul>
        </section>

        <section>
          <h2>4. Использование данных Google API (Google User Data)</h2>
          <p>
            Использование Сервисом информации, полученной через Google APIs (включая Google Sign-In), соответствует
            <a
              href="https://developers.google.com/terms/api-services-user-data-policy"
              target="_blank"
              rel="noopener noreferrer"
              style={{ color: 'var(--accent, #6366f1)', marginLeft: 4 }}
            >
              Google API Services User Data Policy
            </a>
            , включая требования Limited Use (Ограниченного использования). Мы не передаем и не продаем данные учетных записей Google сторонним рекламным сетям и не используем их для обучения публичных моделей без прямого согласия пользователя.
          </p>
        </section>

        <section>
          <h2>5. Защита и хранение информации</h2>
          <p>
            Все соединения между приложением и сервером шифруются с использованием протокола TLS (HTTPS / WSS).
            Пароли хэшируются с использованием стойких алгоритмов с солью (Argon2id / PBKDF2).
            Токены внешних интеграций шифруются алгоритмом AES-256-GCM.
            Доступ к базам данных ограничен закрытой сетью.
          </p>
        </section>

        <section>
          <h2>6. Право на удаление данных (Account Deletion)</h2>
          <p>
            Каждый пользователь имеет право в любой момент полностью и безвозвратно удалить свой аккаунт и все связанные с ним персональные данные:
          </p>
          <ul>
            <li>В приложении: перейдите в <strong>Настройки → Опасная зона → Удалить аккаунт</strong>.</li>
            <li>При подтверждении удаления аккаунт, сессии, токены, задачи и переписки немедленно стираются из базы данных.</li>
            <li>Также вы можете направить запрос на удаление данных на электронную почту службы поддержки: <code>privacy@selfvoy.network</code>.</li>
          </ul>
        </section>

        <section>
          <h2>7. Контакты</h2>
          <p>
            По любым вопросам, связанным с Политикой конфиденциальности или обработкой ваших персональных данных, обращайтесь по адресу:
            <br />
            Электронная почта: <strong>privacy@selfvoy.network</strong>
          </p>
        </section>
      </div>
    </div>
  );
}
