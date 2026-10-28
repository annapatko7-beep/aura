/*
  Буфер обмена и «Поделиться»: общие для страниц владельца и гостевых страниц
  (гостевые грузятся отдельным чанком — без кода страниц приложения).
*/

/** Копирование в буфер: Clipboard API, иначе — через скрытое поле (старые WebView). */
export async function copyText(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    /* нет доступа к буферу — пробуем по-старому */
  }
  try {
    const area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand('copy');
    area.remove();
    return ok;
  } catch {
    return false;
  }
}

export const canShare = () => typeof navigator !== 'undefined' && typeof navigator.share === 'function';
