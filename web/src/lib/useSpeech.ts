import { SpeechRecognition as NativeSpeech } from '@capacitor-community/speech-recognition';
import { useCallback, useEffect, useRef, useState } from 'react';
import { isNative } from '../platform';

interface Recognition {
  lang: string;
  interimResults: boolean;
  onresult: (e: { results: ArrayLike<ArrayLike<{ transcript: string }>> }) => void;
  onend: () => void;
  onerror: () => void;
  start: () => void;
  stop: () => void;
}

type Ctor = new () => Recognition;

function webImpl(): Ctor | undefined {
  // Electron exposes the API but has no recognition service behind it.
  if (typeof window === 'undefined' || window.selfvoyDesktop) return undefined;
  const w = window as unknown as { SpeechRecognition?: Ctor; webkitSpeechRecognition?: Ctor };
  return w.SpeechRecognition ?? w.webkitSpeechRecognition;
}

/**
 * Speech-to-text. On iOS and Android it uses the system recogniser through a Capacitor plugin (the
 * in-app WebView has no Web Speech API); in browsers it uses Web Speech where available. The desktop
 * shell has no recogniser yet, so the mic button is disabled there.
 */
export function useSpeech(onText: (text: string) => void) {
  const [listening, setListening] = useState(false);
  const [supported, setSupported] = useState(() => !isNative && Boolean(webImpl()));
  const web = useRef<Recognition | null>(null);
  const textRef = useRef(onText);
  textRef.current = onText;
  const listeningRef = useRef(false);
  listeningRef.current = listening;
  const mounted = useRef(true);

  // Ушли с экрана — микрофон выключается: распознавание не должно продолжаться
  // в фоне и присылать текст в уже закрытое поле.
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      const r = web.current;
      web.current = null;
      if (r) {
        r.onresult = () => undefined;
        r.onend = r.onerror = () => undefined;
        try {
          r.stop();
        } catch {
          /* уже остановлено */
        }
      }
      if (isNative && listeningRef.current) NativeSpeech.stop().catch(() => undefined);
    };
  }, []);

  useEffect(() => {
    if (!isNative) return;
    NativeSpeech.available()
      .then((r) => setSupported(r.available))
      .catch(() => setSupported(false));
    const subs = [
      NativeSpeech.addListener('partialResults', (d) => mounted.current && d.matches[0] && textRef.current(d.matches[0])),
      NativeSpeech.addListener('listeningState', (d) => mounted.current && setListening(d.status === 'started')),
    ];
    return () => {
      subs.forEach((s) => s.then((h) => h.remove()));
    };
  }, []);

  const toggle = useCallback(async () => {
    if (isNative) {
      if (listening) {
        await NativeSpeech.stop();
        return;
      }
      const perm = await NativeSpeech.requestPermissions();
      if (perm.speechRecognition !== 'granted' || !mounted.current) return;
      setListening(true);
      await NativeSpeech.start({ language: 'ru-RU', partialResults: true, popup: false, maxResults: 1 }).catch(() => setListening(false));
      return;
    }

    const Impl = webImpl();
    if (!Impl) return;
    if (web.current) {
      web.current.stop();
      return;
    }
    const r = new Impl();
    r.lang = 'ru-RU';
    r.interimResults = true;
    r.onresult = (e) => {
      if (web.current === r) textRef.current(Array.from(e.results, (x) => x[0].transcript).join(''));
    };
    r.onend = r.onerror = () => {
      if (web.current !== r) return;
      web.current = null;
      setListening(false);
    };
    web.current = r;
    setListening(true);
    r.start();
  }, [listening]);

  return { supported, listening, toggle };
}
