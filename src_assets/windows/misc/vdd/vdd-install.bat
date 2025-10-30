@echo off

REM 从注册表获取
for /f "delims=" %%i in ('reg query "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v "DisplayVersion"') do set "DisplayVersion=%%i"
echo window display version: %DisplayVersion%

REM 取数据部分的内容
for %%a in (%DisplayVersion%) do set "version=%%a"
echo version: %version%
REM 数据来源：https://github.com/Drawbackz/DevCon-Installer/blob/master/devcon_sources.json
IF "%version%"=="23H2" set "WindowHash=C8FCE4377D8F0D184E5434F9EF1FE1EF9D0E34196A08CD7E5655555FABA21379"
IF "%version%"=="22H2" set "WindowHash=4F0C165C58114790DB7807872DEBD99379321024DB6077F0ED79426CF9C05CA0"
IF "%version%"=="21H2" set "WindowHash=FBD394E4407C6C334B933FF3A0D21A8E28F0EEDE0CFE5FB277287C3F994B5B00"

echo window hash:%WindowHash%

REM 将我们的配置目录写入注册表
rem install
set "DRIVER_DIR=%~dp0\VirtualDisplayDriver"
echo driver directory:%DRIVER_DIR%
rem Get root directory,current in the scripts directory.
for %%I in ("%~dp0\..\..") do set "ROOT_DIR=%%~fI"
set "CONFIG_DIR=%ROOT_DIR%\config"
set "VDD_CONFIG=%CONFIG_DIR%\vdd_settings.xml"
echo driver config path: %VDD_CONFIG%
REM 总是从原始目录中拷贝配置到配置目录，不管是用于恢复还是备份，都是不错的选择
copy "%DRIVER_DIR%\vdd_settings.xml" "%VDD_CONFIG%"
REM 因为默认读的是注册表配置的目录，我们需要将注册表的目录配置一下
reg add "HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver" /v VDDPATH /t REG_SZ /d "%CONFIG_DIR%" /f

REM 安装具体的驱动
powershell.exe -ExecutionPolicy Bypass -File "silent-install.ps1" $WindowHash

REM 初次安装后，还应设置成扩展这些显示器的显示模式
powershell -NoProfile -ExecutionPolicy Bypass -Command "& "C:\Windows\System32\DisplaySwitch.exe" /extend"

REM 安装后，先禁用显示驱动
powershell.exe -ExecutionPolicy Bypass -File "virtual-driver-manager.ps1" disable --silent true
pause