; Compositor Windows 安装包（Inno Setup 6）。
; 用法：ISCC /DSourceDir=<windeployqt 之后的目录> /DOutputDir=<输出目录> installer.iss

#ifndef SourceDir
  #define SourceDir "..\..\dist\Compositor"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#define AppVersion "0.1.0"

[Setup]
AppId={{8C1B6F0E-3B7A-4F57-9D7E-0C5E7A913B2D}
AppName=Compositor
AppVersion={#AppVersion}
AppPublisher=Compositor
DefaultDirName={autopf}\Compositor
DefaultGroupName=Compositor
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=Compositor-Setup-{#AppVersion}-x64
SetupIconFile=compositor.ico
UninstallDisplayIcon={app}\Compositor.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Languages]
; 简体中文语言文件随较新的 Inno Setup 提供；没有时退回英文。
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Compositor"; Filename: "{app}\Compositor.exe"
Name: "{group}\{cm:UninstallProgram,Compositor}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Compositor"; Filename: "{app}\Compositor.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Compositor.exe"; Description: "{cm:LaunchProgram,Compositor}"; Flags: nowait postinstall skipifsilent
