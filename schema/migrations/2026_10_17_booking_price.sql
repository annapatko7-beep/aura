-- Снимок цены записи (bookings.price / bookings.currency): цена одной встречи
-- страницы на момент записи (мин. единицы, как booking_pages.price). Запись по
-- пакету (package_id) — 0: встречи оплачены абонементом. Перенос и отмена цену
-- не меняют. Идемпотентно: колонка добавляется и заполняется только один раз
-- (повторный запуск не перезаписывает снимки текущей ценой страницы).
-- См. docs/DATABASE.md.

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                   WHERE table_schema = current_schema() AND table_name = 'bookings'
                     AND column_name = 'currency') THEN
        ALTER TABLE bookings ADD COLUMN currency TEXT NOT NULL DEFAULT 'RUB'
            CHECK (currency IN ('RUB', 'USD', 'EUR', 'KZT', 'BYN'));
        UPDATE bookings b SET currency = p.currency FROM booking_pages p WHERE p.id = b.page_id;
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                   WHERE table_schema = current_schema() AND table_name = 'bookings'
                     AND column_name = 'price') THEN
        ALTER TABLE bookings ADD COLUMN price INTEGER NOT NULL DEFAULT 0 CHECK (price >= 0);
        -- Старые записи: текущая цена страницы (точнее не узнать), кроме записей по пакету.
        UPDATE bookings b SET price = p.price FROM booking_pages p
            WHERE p.id = b.page_id AND b.package_id IS NULL;
    END IF;
END
$$;
