/*
  Страницы записи и записи гостей владельца (booking.pageList + booking.list) —
  общие для «Сегодня» и «Записи». Загружаются при каждом подключении, обновляются
  по уведомлениям booking.* (гость записался, перенёс, отменил, подтвердил, оплатил).
*/
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { bookingOwnerErrorText } from '../lib/booking';
import { makeBookingApi, type Booking, type BookingApi, type BookingPage } from '../net/bookingApi';
import { isNetworkError } from '../net/socket';
import { useSession } from './session';

export interface OwnerBookings {
  api: BookingApi;
  pages: BookingPage[] | null;
  bookings: Booking[] | null;
  error: string;
  reload: () => Promise<void>;
  /** Ответ сервера после изменения — сразу на экран (затем reload). */
  putBooking: (booking: Booking) => void;
  putPage: (page: BookingPage, isNew?: boolean) => void;
  dropPage: (id: number) => void;
}

export function useOwnerBookings(limit = 100): OwnerBookings {
  const { socket, conn } = useSession();
  const api = useMemo(() => makeBookingApi(socket), [socket]);
  const [pages, setPages] = useState<BookingPage[] | null>(null);
  const [bookings, setBookings] = useState<Booking[] | null>(null);
  const [error, setError] = useState('');
  const seq = useRef(0);

  const reload = useCallback(async () => {
    const mine = ++seq.current;
    try {
      const [p, b] = await Promise.all([api.listPages(), api.list(limit)]);
      if (mine !== seq.current) return;
      setPages(p.pages ?? []);
      setBookings(b.bookings ?? []);
      setError('');
    } catch (e) {
      if (mine !== seq.current) return;
      // Сетевые ошибки не шумим: есть баннер «нет связи», список перезагрузится при переподключении.
      if (!isNetworkError(e)) setError(bookingOwnerErrorText(e));
    }
  }, [api, limit]);

  useEffect(() => {
    if (conn === 'open') void reload();
  }, [conn, reload]);

  useEffect(
    () =>
      socket.onEvent((event) => {
        if (event.event !== 'notification.new') return;
        const kind = String((event.payload as { kind?: unknown }).kind ?? '');
        if (kind.startsWith('booking.')) void reload();
      }),
    [socket, reload],
  );

  // Ответы, пришедшие после ухода со страницы, не применяем.
  useEffect(
    () => () => {
      seq.current += 1;
    },
    [],
  );

  const putBooking = useCallback((booking: Booking) => {
    setBookings((list) => (list ?? []).map((x) => (x.id === booking.id ? booking : x)));
  }, []);
  const putPage = useCallback((page: BookingPage, isNew?: boolean) => {
    setPages((list) => (isNew ? [page, ...(list ?? []).filter((x) => x.id !== page.id)] : (list ?? []).map((x) => (x.id === page.id ? page : x))));
  }, []);
  const dropPage = useCallback((id: number) => setPages((list) => (list ?? []).filter((x) => x.id !== id)), []);

  return { api, pages, bookings, error, reload, putBooking, putPage, dropPage };
}
