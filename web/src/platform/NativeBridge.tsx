import { useEffect, useRef } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { onBackButton } from './native';

const ROOTS = new Set(['/', '/login']);

/**
 * Router-aware native glue: screens open scrolled to the top, and the Android back button walks
 * history instead of closing the app.
 */
export function NativeBridge() {
  const navigate = useNavigate();
  const location = useLocation();
  const path = useRef(location.pathname);
  path.current = location.pathname;

  // Each screen opens at the top, as in a native app; phones scroll the document itself.
  useEffect(() => {
    window.scrollTo(0, 0);
  }, [location.pathname]);

  useEffect(
    () =>
      onBackButton(
        () => navigate(-1),
        // Страница приглашения открывается по ссылке сама по себе — «Назад» с неё сворачивает приложение.
        () => ROOTS.has(path.current) || path.current.startsWith('/i/'),
      ),
    [navigate],
  );
  return null;
}
