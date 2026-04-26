@echo off
:: 设置窗口大小 (88列 x 26行，适配中文显示)
mode con cols=92 lines=37
:: 设置标题
title XYFileSync 服务管理后台
:: 自动请求管理员权限（如果需要，可取消注释）
:: net session >nul 2>&1 || (powershell start -verb runas '%0' & exit)

:MENU
cls
echo.
echo.
echo.                =========================================================
echo                                 XYFileSync 服务管理交互界面
echo                 =========================================================
echo.
echo.                                   [1] 启动服务 (Start)
echo.                                   [2] 停止服务 (Stop)
echo.                                   [3] 重启服务 (Restart)
echo.                                   [4] 查看状态 (Status)
echo.
echo.                                   [5] 安装服务 (Install)
echo.                                   [6] 卸载服务 (Uninstall)
echo.
echo.                                   [7] 更新覆盖 (Update)
echo.
echo.                                   [0] 退出程序 (Exit)
echo.                 =========================================================
echo.
:: 提示语居中对齐
set /p choice=                                       请选择操作数字并按回车: 

if "%choice%"=="7" goto UPDATE
if "%choice%"=="5" goto INSTALL
if "%choice%"=="6" goto UNINSTALL
if "%choice%"=="1" goto START
if "%choice%"=="2" goto STOP
if "%choice%"=="3" goto RESTART
if "%choice%"=="4" goto STATUS
if "%choice%"=="0" exit
goto MENU

:UPDATE
echo.
call "%~dp0A一键复制、清测试数据.bat"
echo.
echo 文件已更新覆盖完成！
pause
goto MENU

:INSTALL
"%~dp0FileSync.exe" --install
pause
goto MENU

:UNINSTALL
net stop XYFileSync >nul 2>&1
"%~dp0FileSync.exe" --uninstall
pause
goto MENU

:START
net start XYFileSync
pause
goto MENU

:STOP
net stop XYFileSync
pause
goto MENU

:RESTART
net stop XYFileSync
net start XYFileSync
pause
goto MENU

:STATUS
echo.
echo 正在查询 XYFileSync 服务状态...
sc query XYFileSync
echo.
echo ---------------------------------------------------------
echo 提示: [RUNNING] 代表运行中, [STOPPED] 代表已停止
echo ---------------------------------------------------------
pause
goto MENU