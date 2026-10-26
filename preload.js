const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  updateBattery: (data) => ipcRenderer.send('battery-update', data)
});