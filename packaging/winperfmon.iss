; winperfmon 安装脚本（Inno Setup 6）
; 正式构建：  "D:\GitHub\winperfmon-toolchain\innosetup\ISCC.exe" packaging\winperfmon.iss
;              → dist\winperfmon-0.3.0-setup.exe（需管理员；含 PawnIO 依赖的安装与卸载）
; 测试构建：  ...ISCC.exe /DTestBuild=1 packaging\winperfmon.iss
;              → dist\winperfmon-0.3.0-setup-test.exe（最低权限、装到用户目录、跳过 PawnIO）
; 载荷：      dist\winperfmon-0.3.0-win64\（由打包步骤先组装好）

#define AppName "winperfmon"
#define AppVersion "0.3.0"
#define AppPublisher "nanji0710"
#define AppExeName "winperfmon.exe"
#define AppUrl "https://github.com/nanji0710/winperfmon"
#define PayloadDir "..\dist\winperfmon-0.3.0-win64"
#define PawnIoSetup "PawnIO_setup_2.2.0.exe"
#define PawnIoVersion "2.2.0.0"

#ifdef TestBuild
  #define SkipPawnIo
#endif

[Setup]
AppId={{8E31C3B4-2C77-4A0F-9B5A-2F3D7E1A6C42}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
#ifdef TestBuild
PrivilegesRequired=lowest
DefaultDirName={localappdata}\winperfmon-test
#else
PrivilegesRequired=admin
DefaultDirName={autopf}\{#AppName}
#endif
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename={#AppName}-{#AppVersion}-setup{#ifdef TestBuild}-test{#endif}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\{#AppExeName}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
CloseApplications=yes
AllowNoIcons=yes
SetupLogging=yes

[Languages]
Name: "chinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
#ifndef SkipPawnIo
Name: "pawnio"; Description: "安装 PawnIO 驱动（Ring0 指标：每核频率、CPU 封装温度与功耗）"; GroupDescription: "依赖组件："; Flags: checkedonce
#endif
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："; Flags: checkedonce
Name: "autostart"; Description: "开机自动启动面板（写入当前用户启动项）"; GroupDescription: "快捷方式："; Flags: unchecked

[Files]
Source: "{#PayloadDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\PresentMon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\modules\*"; DestDir: "{app}\modules"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\使用说明.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\以管理员运行.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\生成诊断报告.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\{#PawnIoSetup}"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\winperfmon"; Filename: "{app}\{#AppExeName}"; Comment: "性能监控面板（免驱模式）"
Name: "{group}\winperfmon（管理员模式）"; Filename: "{app}\以管理员运行.cmd"; Comment: "以管理员启动：额外获得 Ring0 指标"
Name: "{group}\生成诊断报告"; Filename: "{app}\生成诊断报告.cmd"
Name: "{group}\卸载 winperfmon"; Filename: "{uninstallexe}"
Name: "{autodesktop}\winperfmon"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon
Name: "{autodesktop}\winperfmon（管理员模式）"; Filename: "{app}\以管理员运行.cmd"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "winperfmon"; ValueData: """{app}\{#AppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
#ifndef SkipPawnIo
Filename: "{tmp}\{#PawnIoSetup}"; Parameters: "-silent"; StatusMsg: "正在安装 PawnIO 驱动（Ring0 指标依赖）…"; Flags: waituntilterminated; Tasks: pawnio; Check: ShouldInstallPawnIo
#endif
Filename: "{app}\{#AppExeName}"; Description: "立即运行 winperfmon（打开面板）"; Flags: nowait postinstall skipifsilent

[UninstallRun]
#ifndef SkipPawnIo
Filename: "{code:PawnIoExe}"; Parameters: "{code:PawnIoParams}"; RunOnceId: "RemovePawnIO"; Flags: waituntilterminated; Check: ShouldRemovePawnIo
#endif

[UninstallDelete]
Type: filesandordirs; Name: "{app}\docs"
Type: filesandordirs; Name: "{app}\modules"

[Code]
var
  RemovePawnIo: Boolean;

function PosFrom(const Sub, S: String; Start: Integer): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := Start to Length(S) - Length(Sub) + 1 do
    if Copy(S, I, Length(Sub)) = Sub then
    begin
      Result := I;
      Exit;
    end;
end;

function PawnIoInstalled(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO')
         or RegKeyExists(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO');
end;

function PawnIoDisplayVersion(): String;
var
  S: String;
begin
  S := '';
  if not RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO', 'DisplayVersion', S) then
    RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO', 'DisplayVersion', S);
  Result := S;
end;

function PawnIoCommand(): String;
var
  S: String;
begin
  S := '';
  if not RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO', 'QuietUninstallString', S) then
    if not RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO', 'QuietUninstallString', S) then
      if not RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO', 'UninstallString', S) then
        RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO', 'UninstallString', S);
  Result := S;
end;

function PawnIoExe(Param: String): String;
var
  Cmd: String;
  P1, P2: Integer;
begin
  Cmd := Trim(PawnIoCommand());
  P1 := Pos('"', Cmd);
  if P1 > 0 then
  begin
    P2 := PosFrom('"', Cmd, P1 + 1);
    if P2 > P1 then Result := Copy(Cmd, P1 + 1, P2 - P1 - 1)
    else Result := Copy(Cmd, P1 + 1, Length(Cmd));
  end
  else
  begin
    P2 := Pos(' ', Cmd);
    if P2 > 0 then Result := Copy(Cmd, 1, P2 - 1) else Result := Cmd;
  end;
end;

function PawnIoParams(Param: String): String;
var
  Cmd: String;
  P1, P2: Integer;
begin
  Cmd := Trim(PawnIoCommand());
  P1 := Pos('"', Cmd);
  if P1 > 0 then
  begin
    P2 := PosFrom('"', Cmd, P1 + 1);
    if P2 > P1 then Result := Trim(Copy(Cmd, P2 + 1, Length(Cmd)))
    else Result := '';
  end
  else
  begin
    P2 := Pos(' ', Cmd);
    if P2 > 0 then Result := Trim(Copy(Cmd, P2 + 1, Length(Cmd))) else Result := '';
  end;
  if Result = '' then Result := '-uninstall -silent';
end;

function ShouldInstallPawnIo(): Boolean;
begin
#ifdef SkipPawnIo
  Result := False;
  Exit;
#endif
  Result := PawnIoDisplayVersion() <> '{#PawnIoVersion}';
  if not Result then
    Log('PawnIO {#PawnIoVersion} 已安装，跳过驱动安装步骤。');
end;

function ShouldRemovePawnIo(): Boolean;
begin
#ifdef SkipPawnIo
  Result := False;
  Exit;
#endif
  Result := RemovePawnIo;
end;

function InitializeUninstall(): Boolean;
var
  Answer: Integer;
begin
  Result := True;
#ifdef SkipPawnIo
  RemovePawnIo := False;
  Exit;
#endif
  if not PawnIoInstalled() then
  begin
    RemovePawnIo := False;
    Exit;
  end;
  if UninstallSilent() then
  begin
    RemovePawnIo := True;
    Exit;
  end;
  Answer := MsgBox('是否同时卸载 PawnIO 驱动？' + #13#10 + #13#10 +
                   'PawnIO 是 winperfmon 读取 Ring0 指标（每核频率、CPU 封装温度与功耗）所需的驱动。' + #13#10 +
                   '如果其它工具（例如 HWiNFO）也在使用它，请选“否”。' + #13#10 + #13#10 +
                   '无论选哪个，winperfmon 自身都会被完整卸载。',
                   mbConfirmation, MB_YESNO);
  RemovePawnIo := (Answer = IDYES);
end;