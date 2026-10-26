const { app, BrowserWindow, Tray, Menu, ipcMain, nativeImage, session } = require('electron');
const path = require('path');

let tray = null;
let mainWindow = null;

// 生成托盘图标的 Base64 (占位透明图)
const defaultIcon = nativeImage.createFromDataURL('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=');

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 400,
    height: 300,
    show: true, // 默认显示，确保可以弹窗
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true
    }
  });

  mainWindow.loadFile('index.html');

  // 窗口关闭时隐藏，而不是退出程序
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
      { label: '退出', click: () => {
          app.isQuitting = true;
          app.quit();
        } 
      }
    ]);
    tray.setContextMenu(contextMenu);
  };

  updateMenu('电量：未知');

  tray.on('double-click', () => {
    mainWindow.show();
  });
}

// 接收来自 index.html 的电量数据
ipcMain.on('battery-update', (event, data) => {
  const { battery, iconBase64 } = data;
  
  if (iconBase64) {
    const image = nativeImage.createFromDataURL(iconBase64);
    tray.setImage(image);
  }

  if (battery >= 0) {
    tray.setToolTip(`Rapoo 鼠标 — 电量 ${battery}%`);
    // 为了省事，我们这里直接重新构建菜单
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
  // 【关键修复】接管 WebHID 的设备选择请求
  session.defaultSession.on('select-hid-device', (event, details, callback) => {
    // 阻止默认行为，由我们手动放行
    event.preventDefault();
    
    // 自动选择第一个设备，或者可以通过对话框让用户选
    if (details.deviceList && details.deviceList.length > 0) {
      // 给系统一点时间，自动选择第一个（通常就是雷柏接收器）
      callback(details.deviceList[0].deviceId);
    } else {
      callback();
    }
  });

  createWindow();
  createTray();
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    // 保持后台运行
  }
});
