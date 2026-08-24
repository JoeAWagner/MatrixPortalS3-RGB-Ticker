'use strict';

const { contextBridge, ipcRenderer } = require('electron');

// Explicit, minimal surface - the renderer never touches Node or ipcRenderer
// directly.
contextBridge.exposeInMainWorld('ticker', {
  getState: () => ipcRenderer.invoke('getState'),
  getSettings: () => ipcRenderer.invoke('getSettings'),
  saveSettings: (patch) => ipcRenderer.invoke('saveSettings', patch),
  syncNow: () => ipcRenderer.invoke('syncNow'),
  startSync: () => ipcRenderer.invoke('startSync'),
  stopSync: () => ipcRenderer.invoke('stopSync'),
  sendMessage: (text) => ipcRenderer.invoke('sendMessage', text),
  sendRaw: (p) => ipcRenderer.invoke('sendRaw', p),
  testDevice: () => ipcRenderer.invoke('testDevice'),
  testCalendar: () => ipcRenderer.invoke('testCalendar'),
  onLog: (cb) => ipcRenderer.on('log', (_e, entry) => cb(entry)),
  onState: (cb) => ipcRenderer.on('state', (_e, s) => cb(s)),
});
