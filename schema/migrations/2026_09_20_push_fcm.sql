-- Этап 11 (Android): платформа push-устройств 'fcm' (Firebase Cloud Messaging).
-- Применяется к существующим БД; для свежих 'fcm' уже есть в schema.sql.
ALTER TABLE push_devices DROP CONSTRAINT IF EXISTS push_devices_platform_check;
ALTER TABLE push_devices
    ADD CONSTRAINT push_devices_platform_check
    CHECK (platform IN ('apns', 'fcm', 'webhook', 'dev'));
