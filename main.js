const { app, BrowserWindow, Tray, Menu, ipcMain, nativeImage } = require('electron');
const path = require('path');

let tray = null;
let mainWindow = null;

// 生成托盘图标的 Base64 (这里用了一个简单的透明占位图，实际会在 HTML 中通过 Canvas 生成真实图标)
const defaultIcon = nativeImage.createFromDataURL('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=');

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 400,
    height: 300,
    show: false, // 默认隐藏窗口
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

  const contextMenu = Menu.buildFromTemplate([
    { label: '电量：未知', enabled: false, id: 'battery-text' },
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

  // 双击托盘图标打开控制台
  tray.on('double-click', () => {
    mainWindow.show();
  });
}

// 接收来自 index.html 的电量数据
ipcMain.on('battery-update', (event, data) => {
  const { battery, iconBase64 } = data;
  
  // 更新托盘图标
  if (iconBase64) {
    const image = nativeImage.createFromDataURL(iconBase64);
    tray.setImage(image);
  }

  // 更新鼠标悬停提示和右键菜单
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
  createWindow();
  createTray();
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    // 保持后台运行
  }
});