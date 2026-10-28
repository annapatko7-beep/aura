// Exposes only what the UI needs; no Node APIs reach the page.
//  - selfvoyDesktop: the OS, so CSS can leave room for window buttons;
//  - selfvoySecure: token storage encrypted by the OS (safeStorage in the main process;
//    the main process serves it only to the app's own page).
const { contextBridge, ipcRenderer } = require('electron');

// A file dropped onto the window must not replace the app (the main process blocks such
// navigation too): swallow drops that nothing in the page handled.
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => e.preventDefault());

contextBridge.exposeInMainWorld('selfvoyDesktop', {
  platform: process.platform,
});

contextBridge.exposeInMainWorld('selfvoySecure', {
  get: (key) => ipcRenderer.invoke('secure:get', String(key)),
  set: (key, value) => ipcRenderer.invoke('secure:set', String(key), String(value)),
  remove: (key) => ipcRenderer.invoke('secure:remove', String(key)),
  encrypted: () => ipcRenderer.invoke('secure:encrypted'),
});
