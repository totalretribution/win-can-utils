#ifndef MyAppVersion
  #define MyAppVersion "dev"
#endif

#define MyAppName      "WinCAN"
#define MyAppPublisher "WinCAN"
#define MyAppURL       "https://github.com/totalretribution/win-can-utils"

[Setup]
AppId={{8F4A1C2E-3B5D-4E6F-9A0B-1C2D3E4F5A6B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=wincan-setup-{#MyAppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "installservice"; \
    Description: "Install WinCAN as a Windows Service (starts automatically on boot)"; \
    GroupDescription: "Windows Service:"; \
    Flags: checkedonce
Name: "installservice\startservice"; \
    Description: "Start the service immediately after installation"; \
    Flags: checkedonce

[Files]
Source: "..\build\wincan_server.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\candump.exe";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\cangen.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\cansend.exe";       DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Add install dir to system PATH
Root: HKLM; \
    Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; \
    Check: NeedsAddPath(ExpandConstant('{app}')); \
    Flags: preservestringtype

[Run]
; Install then optionally start the service (admin already elevated)
Filename: "{app}\wincan_server.exe"; Parameters: "--install"; \
    Flags: runhidden waituntilterminated; \
    StatusMsg: "Installing WinCAN service..."; \
    Tasks: installservice
Filename: "{app}\wincan_server.exe"; Parameters: "--start"; \
    Flags: runhidden waituntilterminated; \
    StatusMsg: "Starting WinCAN service..."; \
    Tasks: installservice\startservice

[UninstallRun]
; Stop and remove service before files are deleted
Filename: "{app}\wincan_server.exe"; Parameters: "--uninstall"; \
    Flags: runhidden waituntilterminated; \
    RunOnceId: "UninstallService"

[Code]

// Returns true if AppDir is not already in the system PATH
function NeedsAddPath(AppDir: string): boolean;
var
  Path: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', Path) then begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(AppDir) + ';',
               ';' + Uppercase(Path) + ';') = 0;
end;

// Remove AppDir from system PATH on uninstall
procedure RemovePath(AppDir: string);
var
  Path:    string;
  NewPath: string;
  P:       Integer;
  Entry:   string;
  Sep:     string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', Path) then exit;

  // Ensure trailing semicolon so every entry ends with one
  if (Length(Path) > 0) and (Path[Length(Path)] <> ';') then
    Path := Path + ';';

  NewPath := '';
  while Length(Path) > 0 do begin
    P := Pos(';', Path);
    Entry := Copy(Path, 1, P - 1);
    Path  := Copy(Path, P + 1, Length(Path));
    if Uppercase(Entry) <> Uppercase(AppDir) then begin
      if NewPath <> '' then Sep := ';' else Sep := '';
      NewPath := NewPath + Sep + Entry;
    end;
  end;

  RegWriteStringValue(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', NewPath);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemovePath(ExpandConstant('{app}'));
end;
