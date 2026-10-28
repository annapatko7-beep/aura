import { ArrowUp, Mic, Square } from 'lucide-react';
import { useEffect, useRef, useState, type FormEvent } from 'react';
import { useSpeech } from '../lib/useSpeech';
import { tap } from '../platform/native';

/**
 * Строка запроса к Selfvoy: поле, голос и отправка. Голос — в один клик: нажали,
 * сказали, распознанный текст уходит сам, как только распознавание закончилось.
 */
export function CommandBar({
  onSubmit,
  placeholder = 'Что нужно сделать?',
  busy,
  autoFocus,
}: {
  onSubmit: (text: string) => void;
  placeholder?: string;
  busy?: boolean;
  autoFocus?: boolean;
}) {
  const [text, setText] = useState('');
  const textRef = useRef(text);
  textRef.current = text;
  const busyRef = useRef(busy);
  busyRef.current = busy;
  const speech = useSpeech(setText);
  const wasListening = useRef(false);

  useEffect(() => {
    // Распознавание закончилось — отправляем то, что услышали. Если предыдущий
    // запрос ещё выполняется, текст остаётся в поле: отправить можно вручную.
    if (wasListening.current && !speech.listening && textRef.current.trim() && !busyRef.current) {
      onSubmit(textRef.current.trim());
      setText('');
    }
    wasListening.current = speech.listening;
  }, [speech.listening, onSubmit]);

  const submit = (e: FormEvent) => {
    e.preventDefault();
    const value = text.trim();
    if (!value || busy) return;
    tap();
    onSubmit(value);
    setText('');
  };

  return (
    <form className="command" onSubmit={submit}>
      <input value={text} onChange={(e) => setText(e.target.value)} placeholder={speech.listening ? 'Говорите…' : placeholder} aria-label="Запрос к Selfvoy" autoFocus={autoFocus} enterKeyHint="send" />
      {speech.supported && (
        <button type="button" className={`icon-btn mic ${speech.listening ? 'listening' : ''}`} onClick={speech.toggle} aria-label={speech.listening ? 'Остановить запись' : 'Сказать голосом'} title={speech.listening ? 'Остановить' : 'Сказать голосом'}>
          {speech.listening ? <Square size={15} /> : <Mic size={17} />}
        </button>
      )}
      <button type="submit" className="icon-btn send" disabled={!text.trim() || busy} aria-label="Отправить">
        <ArrowUp size={17} />
      </button>
    </form>
  );
}
