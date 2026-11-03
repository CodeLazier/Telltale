# Telltale

Windows 11 上的微软拼音状态提示。前台程序在允许列表里（默认是 Edge、Cursor、VS Code），并且当前是中文输入时，在文本插入符下方显示一个蓝色「中」，算是当场告一状。英文模式或其他程序里什么都不显示。

徽标不挡住鼠标，点击会落到下面的文字上，也不会出现在 Alt+Tab 里。

## 编译

需要 64 位 Windows 11，以及 [MSYS2](https://www.msys2.org/) 的 UCRT64 工具链。刚装好的系统没有 `g++` 和 `windres`，`build.bat` 也不会自己下载，直接双击会退出。

1. 安装 MSYS2。默认目录是 `C:\msys64`。
2. 从开始菜单打开 **MSYS2 UCRT64**，执行：

    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-gcc
    ```

    `windres` 会随这套工具链一起装上。

3. 把 `C:\msys64\ucrt64\bin` 加进 PATH：设置 → 系统 → 系统信息 → 高级系统设置 → 环境变量。加的是 `ucrt64\bin`。`usr\bin` 里的 `g++` 不能在普通命令行里用。MSYS2 如果装在别的目录，就改成那个目录下的 `ucrt64\bin`。
4. 关掉已经打开的命令行，新开一个，确认能找到编译器：

    ```bat
    where g++
    where windres
    ```

5. 进入项目目录，执行：

    ```bat
    build.bat
    ```

成功后程序在：

```
%LOCALAPPDATA%\Programs\Telltale\telltale.exe
```

也就是 `C:\Users\你的用户名\AppData\Local\Programs\Telltale\telltale.exe`。项目目录里不会留下编译产物。这是一个单独的 exe，不需要再装运行库。

`build.bat` 使用优化编译（`-O2`，不带调试符号），编出来就是日常使用的版本。

编译前先从托盘退出正在运行的 Telltale。exe 被占用时，链接会失败。

## 使用

运行上面的 exe。托盘里出现太阳图标。任务栏上看不到时，打开「隐藏的图标」找一下。

右键托盘图标：

| 菜单       | 作用                                                     |
| ---------- | -------------------------------------------------------- |
| 显示状态标 | 总开关。取消后收起徽标，并停止检测进程和输入法           |
| 仅指定进程 | 默认勾选，只在配置列出的程序里显示。取消后所有程序都显示 |
| 全屏时暂停 | 默认勾选。前台全屏时收起徽标。取消后只按原来的焦点判断   |
| 开机自启   | 默认勾选。在启动文件夹放一个快捷方式，不写注册表         |
| 退出       | 退出程序                                                 |

双击托盘图标同样是暂停或恢复。暂停期间不再检测。

开机自启的快捷方式在：

```
%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Telltale.lnk
```

取消「开机自启」会删掉这个快捷方式。

徽标跟的是插入符，不是鼠标。在输入框里、微软拼音为中文时出现；切到英文，或点到网页正文、侧栏、工具栏时会收起。靠近屏幕底边时，徽标改到插入符上方。多显示器和系统缩放下可以正常使用。

「全屏时暂停」勾着时，前台进入全屏会收起徽标，退出全屏后自动恢复。游戏的独占全屏、无边框全屏，以及播放器的全屏都算。普通的最大化窗口不会触发。取消这项后，全屏窗口只要还有输入焦点，徽标仍按原来的规则显示。

### 允许哪些程序

配置文件和 exe 在同一目录，第一次运行时自动生成：

`%LOCALAPPDATA%\Programs\Telltale\telltale.ini`

```ini
[Telltale]
Enabled=1
OnlyEdge=1
PauseFullscreen=1
AutoStart=1
Debug=0
Processes=msedge.exe,Cursor.exe,Code.exe
```

- `Enabled`：是否显示徽标。为 `0` 时收起徽标并停止检测
- `OnlyEdge`：为 `1` 时只对 `Processes` 里的程序生效，为 `0` 时所有程序都生效
- `PauseFullscreen`：为 `1` 时全屏暂停，为 `0` 时不做全屏判断
- `AutoStart`：是否开机自启
- `Debug`：为 `1` 时把运行记录写到安装目录的 `telltale.exe.log`，为 `0` 时不写文件。没有命令行开关
- `Processes`：进程名，用逗号、分号或竖线分隔。写 `Code.exe` 即可，写完整路径或省掉 `.exe` 也可以，不区分大小写

旧配置里如果少了 `Processes`、`PauseFullscreen` 或 `Debug`，下次启动会补上。前两项默认开启，`Debug` 默认关闭。

加上记事本和 Chrome：

```ini
Processes=msedge.exe,Cursor.exe,Code.exe,notepad.exe,chrome.exe
```

托盘菜单和这份配置改的是同一组开关。在菜单里勾选会立刻生效，并写回 ini。直接改 ini（包括 `Processes` 和 `Debug`）要退出程序再打开。`Processes` 留空，并且「仅指定进程」仍勾着时，任何程序都不显示。

要看运行记录，把 `Debug` 改成 `1`，退出后重新打开。日志在 exe 同目录的 `telltale.exe.log`。改回 `0` 并重新打开后不再写入。

## 注意事项

- 请运行安装目录里的那份 exe。不要在「下载」文件夹或项目目录里再留一份来运行。这些位置常常带着浏览器下载留下的低完整性标记，从这里启动的程序无法显示托盘。这时会弹出提示并退出，不会留在后台。
- 重新编译前先从托盘退出，否则新的 exe 写不进去。
- 只支持微软拼音。系统里切到其他中文输入法时，徽标不会出现。
- 对方程序如果「以管理员身份运行」，普通权限的 Telltale 读不到它的输入状态。
- 正在查询的程序若短暂无响应，徽标保持上一次的中文或英文状态，托盘菜单仍可使用。

## 局限性

- 默认只包含 Edge（`msedge.exe`）、Cursor（`Cursor.exe`）和 VS Code（`Code.exe`）。其他程序要写进 `Processes`，或者取消「仅指定进程」。
- 只识别微软拼音的中文模式，其他中文输入法未适配。
- 管理员权限运行的程序里不会显示徽标。
- 取消「显示状态标」或双击暂停后，程序不再检测，因此这段时间里的中英文切换要等恢复后才会反映出来。
- 程序只判断当前是不是中文输入，不记录按键，不往其他程序里写入内容，也不联网。

## 卸载

1. 托盘菜单里选「退出」。
2. 删除 `%LOCALAPPDATA%\Programs\Telltale`。这里面有 exe、ini，以及 `Debug=1` 时生成的日志。
3. 如果开过开机自启，删除启动文件夹里的 `Telltale.lnk`。
4. 如果用过改名前的 ImeTip，再删掉 `%LOCALAPPDATA%\Programs\ImeTip`、启动文件夹里的 `ImeTip.lnk`，以及注册表里的 `HKCU\Software\ImeTip` 和 Run 下的 `ImeTip`。当前版本不写注册表。
