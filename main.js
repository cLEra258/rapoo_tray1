const { app, BrowserWindow, Tray, Menu, ipcMain, nativeImage, session } = require('electron');
const path = require('path');

let tray = null;
let mainWindow = null;

// 默认透明图标
const defaultIcon = nativeImage.createFromDataURL('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=');

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 500,
    height: 400,
    show: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true
    }
  });

  mainWindow.loadFile('index.html');
  
  // 自动打开开发者工具，方便查看 console.log
  mainWindow.webContents.openDevTools();

  mainWindow.on('close', (event) => {
    if (!app.isQuitting) {
      event.preventDefault();
      mainWindow.hide();
    }
  });
}

function createTray() {
  tray = new Tray(defaultIcon);
  tray.setToolTip('Rapoo 鼠标：等待数据…');

  const updateMenu = (batteryText) => {
    const contextMenu = Menu.buildFromTemplate([
      { label: batteryText, enabled: false },
      { type: 'separator' },
      { label: '显示控制台', click: () => mainWindow.show() },
      { type: 'separator' },
      { label: '退出', click: () => { app.isQuitting = true; app.quit(); } }
    ]);
    tray.setContextMenu(contextMenu);
  };
  
  updateMenu('电量：未知');

  tray.on('double-click', () => mainWindow.show());
}

ipcMain.on('battery-update', (event, data) => {
  const { battery, iconBase64 } = data;
  
  if (iconBase64) {
    const image = nativeImage.createFromDataURL(iconBase64);
    tray.setImage(image);
  }

  if (battery >= 0) {
    tray.setToolTip(`Rapoo 鼠标 — 电量 ${battery}%`);
    const contextMenu = Menu.buildFromTemplate([
      { label: `电量：${battery}%`, enabled: false },
      { type: 'separator' },
      { label: '显示控制台', click: () => mainWindow.show() },
      { type: 'separator' },
      { label: '退出', click: () => { app.isQuitting = true; app.quit(); } }
    ]);
    tray.setContextMenu(contextMenu);
  } else {
    tray.setToolTip('Rapoo 鼠标 — 等待数据…');
    tray.setImage(defaultIcon);
  }
});

app.whenReady().then(() => {
  // 【核心修复】处理 WebHID 权限和自动选择设备
  const defaultSession = session.defaultSession;

  // 1. 允许网页请求 HID 权限
  defaultSession.setPermissionCheckHandler((wc, permission) => {
    if (permission === 'hid') return true;
    return false;
  });

  defaultSession.setDevicePermissionHandler((details) => {
    return true;
  });

  // 2. 接管选择事件，防止 Promise 挂起
  defaultSession.on('select-hid-device', (event, details, callback) => {
    event.preventDefault();
    
    console.log('=== 触发了 select-hid-device ===');
    console.log('系统枚举到的 HID 设备列表:', details.deviceList);
    
    if (details.deviceList && details.deviceList.length > 0) {
      // 优先查找雷柏 VID: 0x24AE (9418)
      const rapooDev = details.deviceList.find(d => d.vendorId === 0x24AE);
      if (rapooDev) {
        console.log('找到雷柏设备，自动选择:', rapooDev.deviceId);
        callback(rapooDev.deviceId);
      } else {
        console.log('未找到雷柏 VID，暂选第一个设备:', details.deviceList[0].deviceId);
        callback(details.deviceList[0].deviceId);
      }
    } else {
      console.log('⚠️ 未发现任何 HID 设备，返回空');
      callback(); 
    }
  });

  createWindow();
  createTray();
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
  }
});
