// Desktop shell for Selfvoy (macOS, Windows, Linux). Loads the built web app from dist/ in a
// sandboxed window; in development (unpackaged app only) set SELFVOY_DEV_URL to load the Vite
// dev server instead.
//
// Security model: the page gets window.selfvoySecure (the login tokens), so only the app's own
// page may ever be loaded in the window. Navigation is allowed to the exact dist/index.html
// (any hash — the app uses a hash router) or, in development, to the dev server's origin;
// anything else (a dropped file, a link, a redirect) is blocked. IPC handlers check that the
// call comes from that page, and web permissions are denied except the microphone.
const { app, BrowserWindow, Menu, shell, nativeTheme, ipcMain, safeStorage, session } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

const isMac = process.platform === 'darwin';
// Фон окна до загрузки страницы — под тему системы (как токены --bg в global.css).
const bg = () => (nativeTheme.shouldUseDarkColors ? '#09080f' : '#f4f3fa');
const symbolColor = () => (nativeTheme.shouldUseDarkColors ? '#e6e4f0' : '#1f1d2b');
const APP_FILE = path.join(__dirname, '..', 'dist', 'index.html');
const APP_URL = pathToFileURL(APP_FILE).href;

/** Dev server origin (http(s) only), honoured only in an unpackaged app. */
function devOrigin() {
  if (app.isPackaged || !process.env.SELFVOY_DEV_URL) return null;
  try {
    const url = new URL(process.env.SELFVOY_DEV_URL);
    return url.protocol === 'http:' || url.protocol === 'https:' ? url.origin : null;
  } catch {
    return null;
  }
}
const DEV_ORIGIN = devOrigin();

/** True only for the app's own page: the exact dist/index.html (hash ignored) or the dev origin. */
function isAppUrl(raw) {
  let url;
  try {
    url = new URL(raw);
  } catch {
    return false;
  }
  if (DEV_ORIGIN) return url.origin === DEV_ORIGIN;
  if (url.protocol !== 'file:') return false;
  url.hash = '';
  return url.href === APP_URL;
}

function openExternal(raw) {
  try {
    const url = new URL(raw);
    if (url.protocol === 'http:' || url.protocol === 'https:') shell.openExternal(url.href);
  } catch {
    /* not a URL — ignore */
  }
}

/** IPC calls are served only to the app's own top-level page in one of our windows. */
function fromApp(event) {
  const frame = event.senderFrame;
  if (!frame || frame.parent) return false;
  if (!BrowserWindow.fromWebContents(event.sender)) return false;
  return isAppUrl(frame.url);
}

nativeTheme.themeSource = 'system';

function syncWindowTheme(win, isDark) {
  if (!win || win.isDestroyed()) return;
  const dark = typeof isDark === 'boolean' ? isDark : nativeTheme.shouldUseDarkColors;
  const newBg = dark ? '#09080f' : '#f4f3fa';
  const newSymbol = dark ? '#e6e4f0' : '#1f1d2b';
  try {
    win.setBackgroundColor(newBg);
    if (!isMac && typeof win.setTitleBarOverlay === 'function') {
      win.setTitleBarOverlay({
        color: '#00000000',
        symbolColor: newSymbol,
        height: 32,
      });
    }
  } catch {
    /* window might be closing or titleBarOverlay not supported */
  }
}

nativeTheme.on('updated', () => {
  for (const win of BrowserWindow.getAllWindows()) {
    syncWindowTheme(win);
  }
});

function createWindow() {
  const win = new BrowserWindow({
    width: 1360,
    height: 880,
    minWidth: 380,
    minHeight: 620,
    backgroundColor: bg(),
    show: false,
    title: 'Selfvoy',
    // The app draws its own glass chrome: hide the title bar but keep native window controls.
    titleBarStyle: 'hidden',
    ...(isMac
      ? { trafficLightPosition: { x: 16, y: 14 } }
      : {
          titleBarOverlay: {
            color: '#00000000',
            symbolColor: symbolColor(),
            height: 32,
          },
        }),
    icon: path.join(__dirname, '..', 'build', 'icon.png'),
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      spellcheck: false,
    },
  });

  win.once('ready-to-show', () => win.show());

  // Links to other sites open in the system browser, never inside the app window.
  win.webContents.setWindowOpenHandler(({ url }) => {
    openExternal(url);
    return { action: 'deny' };
  });
  // Only the app's own page may load here: a dropped .html file or any other file:// page
  // would otherwise get window.selfvoySecure and read the tokens.
  const guard = (e, legacyUrl) => {
    const url = e.url || legacyUrl;
    if (isAppUrl(url)) return;
    e.preventDefault();
    openExternal(url);
  };
  win.webContents.on('will-navigate', guard);
  win.webContents.on('will-redirect', guard);
  win.webContents.on('will-frame-navigate', (e) => {
    if (!e.isMainFrame && !isAppUrl(e.url)) e.preventDefault();
  });

  if (DEV_ORIGIN) win.loadURL(process.env.SELFVOY_DEV_URL);
  else win.loadFile(APP_FILE);
}

// Web permissions: the microphone (voice input) for the app's own page; everything else denied.
function registerPermissions() {
  const ses = session.defaultSession;
  ses.setPermissionRequestHandler((wc, permission, callback, details) => {
    const url = (details && details.requestingUrl) || wc.getURL();
    const media = permission === 'media' && !((details && details.mediaTypes) || []).includes('video');
    callback(Boolean(media && isAppUrl(url)));
  });
  ses.setPermissionCheckHandler((wc, permission, requestingOrigin, details) => {
    if (permission !== 'media') return false;
    if (details && details.mediaType && details.mediaType !== 'audio') return false;
    const url = (details && details.requestingUrl) || (wc && wc.getURL()) || '';
    return isAppUrl(url);
  });
}

// No <webview> tags, no extra windows from any web contents.
app.on('web-contents-created', (_e, contents) => {
  contents.on('will-attach-webview', (e) => e.preventDefault());
  contents.setWindowOpenHandler(({ url }) => {
    openExternal(url);
    return { action: 'deny' };
  });
});

// Token storage for the renderer (window.selfvoySecure): values are encrypted by the OS
// (Keychain / DPAPI / libsecret / KWallet) and kept in userData. Without an OS keyring
// (bare Linux) safeStorage either is unavailable (values stored as-is) or falls back to
// "basic_text" — a hard-coded key, i.e. obfuscation, not encryption. Either way the UI is
// told the tokens are NOT encrypted.
const secureFile = () => path.join(app.getPath('userData'), 'secure.json');

function readSecure() {
  try {
    return JSON.parse(fs.readFileSync(secureFile(), 'utf8'));
  } catch {
    return {};
  }
}

function writeSecure(data) {
  fs.mkdirSync(path.dirname(secureFile()), { recursive: true });
  fs.writeFileSync(secureFile(), JSON.stringify(data), { mode: 0o600 });
}

/** Real OS-backed encryption (not Linux "basic_text" with a built-in key). */
function osEncryption() {
  if (!safeStorage.isEncryptionAvailable()) return false;
  if (process.platform === 'linux' && typeof safeStorage.getSelectedStorageBackend === 'function') {
    const backend = safeStorage.getSelectedStorageBackend();
    return backend !== 'basic_text' && backend !== 'unknown';
  }
  return true;
}

const KEY_RE = /^[a-z][a-z0-9._-]{0,63}$/i;

function registerSecureStore() {
  const canEncrypt = () => safeStorage.isEncryptionAvailable();
  const checked = (handler) => (event, ...args) => {
    if (!fromApp(event)) throw new Error('forbidden');
    const [key] = args;
    if (args.length && (typeof key !== 'string' || !KEY_RE.test(key))) throw new Error('bad key');
    return handler(...args);
  };
  ipcMain.handle('secure:get', checked((key) => {
    const data = readSecure();
    const entry = Object.prototype.hasOwnProperty.call(data, key) ? data[key] : null;
    if (!entry) return null;
    try {
      return entry.enc ? safeStorage.decryptString(Buffer.from(entry.value, 'base64')) : entry.value;
    } catch {
      return null;
    }
  }));
  ipcMain.handle('secure:set', checked((key, value) => {
    if (typeof value !== 'string' || value.length > 64 * 1024) throw new Error('bad value');
    const data = readSecure();
    data[key] = canEncrypt()
      ? { enc: true, value: safeStorage.encryptString(value).toString('base64') }
      : { enc: false, value };
    writeSecure(data);
  }));
  ipcMain.handle('secure:remove', checked((key) => {
    const data = readSecure();
    delete data[key];
    writeSecure(data);
  }));
  ipcMain.handle('secure:encrypted', checked(() => osEncryption()));
}

function registerThemeHandlers() {
  ipcMain.handle('theme:update', (event, { isDark } = {}) => {
    if (!fromApp(event)) throw new Error('forbidden');
    const win = BrowserWindow.fromWebContents(event.sender);
    if (win) syncWindowTheme(win, typeof isDark === 'boolean' ? isDark : undefined);
  });
}

function buildMenu() {
  const template = [
    ...(isMac ? [{ role: 'appMenu' }] : []),
    { role: 'editMenu' },
    { role: 'viewMenu' },
    { role: 'windowMenu' },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

// The app used to be called Aura: its profile (localStorage with the server address and
// settings, secure.json with the login tokens) lives in a userData folder named after the
// app. On the first launch under the new name copy the old folder over, so users stay
// signed in; the page then renames its "aura.*" storage keys itself. Must run before
// anything (the single-instance lock included) creates the new folder.
function migrateUserData() {
  try {
    const target = app.getPath('userData');
    if (fs.existsSync(target)) return;
    const appData = app.getPath('appData');
    const legacy = ['Aura', 'aura-web'].map((name) => path.join(appData, name)).find((dir) => dir !== target && fs.existsSync(dir));
    if (legacy) fs.cpSync(legacy, target, { recursive: true });
  } catch {
    /* start with a fresh profile: the user signs in again */
  }
}
migrateUserData();

function setupAutoUpdater() {
  if (!app.isPackaged) return;
  try {
    const { autoUpdater } = require('electron-updater');
    autoUpdater.autoDownload = true;
    autoUpdater.autoInstallOnAppQuit = true;
    autoUpdater.checkForUpdatesAndNotify().catch(() => {});
  } catch {
    /* autoUpdater not available or offline */
  }
}

app.setAppUserModelId('app.selfvoy.network');

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    const [win] = BrowserWindow.getAllWindows();
    if (win) {
      if (win.isMinimized()) win.restore();
      win.focus();
    }
  });

  app.whenReady().then(() => {
    registerSecureStore();
    registerPermissions();
    buildMenu();
    createWindow();
    setupAutoUpdater();
    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
  });

  app.on('window-all-closed', () => {
    if (!isMac) app.quit();
  });
}
