import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { HashRouter, matchPath, Navigate, Route, Routes, useLocation } from 'react-router-dom';
import { AppShell } from './components/AppShell';
import { ConnectionScreen } from './components/ConnectionScreen';
import { lazyNamed, LazyOutlet } from './components/Lazy';
import { Spinner } from './components/ui';
import { NativeBridge } from './platform/NativeBridge';
import { initPlatform } from './platform/native';
import { migrateLegacyStorage } from './net/storageMigration';
import { initTheme } from './state/theme';
import { ForgotPassword, Login, Signup, TwoFactor, VerifyEmail } from './pages/Auth';
import { DataProvider } from './state/data';
import { SessionProvider, useSession } from './state/session';
import '@fontsource-variable/manrope';
import './styles/global.css';

// Обработка OAuth callback при возврате без hash (например, http://localhost:5173/oauth/callback?code=...)
if (typeof window !== 'undefined' && (window.location.pathname === '/oauth/callback' || window.location.pathname.startsWith('/oauth/callback'))) {
  const search = window.location.search;
  window.location.replace(`${window.location.origin}/#/oauth/callback${search}`);
}

// Ключи «aura.*» → «selfvoy.*» до первого чтения хранилища (вход и настройки сохраняются).
migrateLegacyStorage();
initPlatform();
initTheme();

// Страницы — отдельными чанками: гостевые ссылки (#/i, #/b, #/m, #/p) не тянут код приложения,
// а настройки (QR-код) и админка грузятся, только когда их открыли. Вход — в основном чанке.
const Admin = lazyNamed(() => import('./pages/Admin'), 'Admin');
const Chat = lazyNamed(() => import('./pages/Chat'), 'Chat');
const Contacts = lazyNamed(() => import('./pages/Contacts'), 'Contacts');
const ContactChat = lazyNamed(() => import('./pages/Contacts'), 'ContactChat');
const Home = lazyNamed(() => import('./pages/Home'), 'Home');
const Inbox = lazyNamed(() => import('./pages/Inbox'), 'Inbox');
const More = lazyNamed(() => import('./pages/More'), 'More');
const BookingPage = lazyNamed(() => import('./pages/Booking'), 'BookingPage');
const BookingGuest = lazyNamed(() => import('./pages/BookingGuest'), 'BookingGuest');
const BookingManage = lazyNamed(() => import('./pages/BookingGuest'), 'BookingManage');
const InviteGuest = lazyNamed(() => import('./pages/InviteGuest'), 'InviteGuest');
const PackageGuest = lazyNamed(() => import('./pages/PackageGuest'), 'PackageGuest');
const OAuthCallback = lazyNamed(() => import('./pages/OAuthCallback'), 'OAuthCallback');
const Privacy = lazyNamed(() => import('./pages/Privacy'), 'Privacy');
const Terms = lazyNamed(() => import('./pages/Terms'), 'Terms');
const Packages = lazyNamed(() => import('./pages/Packages'), 'Packages');
const Negotiations = lazyNamed(() => import('./pages/Negotiation'), 'Negotiations');
const NegotiationView = lazyNamed(() => import('./pages/Negotiation'), 'NegotiationView');
const Settings = lazyNamed(() => import('./pages/Settings'), 'Settings');
const Tasks = lazyNamed(() => import('./pages/Tasks'), 'Tasks');

/*
  Публичные страницы гостя: и без входа, и со входом, вне оболочки и без
  переадресаций на вход; о связи сообщают сами. '/p/:token' — страница пакета
  занятий (абонемента) гостя, F5.
*/
const GUEST_ROUTES = [
  { path: '/i/:token', element: <InviteGuest /> },
  { path: '/b/:slug', element: <BookingGuest /> },
  { path: '/m/:token', element: <BookingManage /> },
  { path: '/p/:token', element: <PackageGuest /> },
  { path: '/oauth/callback', element: <OAuthCallback /> },
  { path: '/privacy', element: <Privacy /> },
  { path: '/terms', element: <Terms /> },
] as const;

function useGuestRoute(): boolean {
  const { pathname } = useLocation();
  return GUEST_ROUTES.some((r) => matchPath(r.path, pathname) !== null);
}

function Root() {
  const { status, conn, everConnected, connError } = useSession();
  const guest = useGuestRoute();

  if (guest) {
    return (
      <Routes>
        <Route element={<LazyOutlet full />}>
          {GUEST_ROUTES.map((r) => (
            <Route key={r.path} path={r.path} element={r.element} />
          ))}
        </Route>
      </Routes>
    );
  }

  if (status === 'booting') {
    // Токены есть, профиля в кэше нет — ждём сервер. Если он не отвечает, вместо
    // вечной заставки — экран ожидания с «Повторить» и сменой адреса.
    if (conn !== 'open' && connError) return <ConnectionScreen />;
    return (
      <div className="gate">
        <Spinner />
      </div>
    );
  }

  if (status === 'signedOut') {
    // Без сервера войти нельзя: вместо формы, которая упадёт с «Network Error», — экран ожидания.
    if (conn !== 'open' && !everConnected) return <ConnectionScreen />;
    return (
      <Routes>
        <Route path="/login" element={<Login />} />
        <Route path="/signup" element={<Signup />} />
        <Route path="/verify" element={<VerifyEmail />} />
        <Route path="/2fa" element={<TwoFactor />} />
        <Route path="/forgot" element={<ForgotPassword />} />
        <Route path="*" element={<Navigate to="/login" replace />} />
      </Routes>
    );
  }

  return (
    <DataProvider>
      <Routes>
        <Route element={<AppShell />}>
          {/* Меню оболочки на экране сразу, страница подгружается внутри неё. */}
          <Route element={<LazyOutlet />}>
            <Route index element={<Home />} />
            <Route path="selfvoy" element={<Chat />} />
            <Route path="negotiations" element={<Negotiations />} />
            <Route path="negotiations/:id" element={<NegotiationView />} />
            <Route path="booking" element={<BookingPage />} />
            <Route path="packages" element={<Packages />} />
            <Route path="tasks" element={<Tasks />} />
            <Route path="contacts" element={<Contacts />} />
            <Route path="contacts/:chatId" element={<ContactChat />} />
            <Route path="inbox" element={<Inbox />} />
            <Route path="more" element={<More />} />
            <Route path="settings" element={<Settings />} />
            <Route path="admin" element={<Admin />} />
          </Route>
        </Route>
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </DataProvider>
  );
}

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <SessionProvider>
      <HashRouter>
        <div className="titlebar" aria-hidden="true" />
        <NativeBridge />
        <Root />
      </HashRouter>
    </SessionProvider>
  </StrictMode>,
);
