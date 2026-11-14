Dim edgeFlags
' Launch a local HTML animation in a full-screen Edge app window.
' Place this file next to the HTML file (default: V2.html).
' The slide button must hyperlink to this file with a RELATIVE path.
Dim fso, sh, base, html, edge, url, temp, profile
Set fso = CreateObject("Scripting.FileSystemObject")
base = fso.GetParentFolderName(WScript.ScriptFullName)
html = base & "\V2.html"
If Not fso.FileExists(html) Then
  MsgBox "V2.html not found next to the launcher.", 16, "HTML Launcher"
  WScript.Quit 1
End If
edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
If Not fso.FileExists(edge) Then edge = "C:\Program Files\Microsoft\Edge\Application\msedge.exe"
If Not fso.FileExists(edge) Then edge = "msedge.exe"
url = "file:///" & Replace(html, "\", "/")
temp = fso.GetSpecialFolder(2)
profile = temp & "\html-ppt-edge-profile"
Set sh = CreateObject("WScript.Shell")
edgeFlags = "--no-first-run --no-default-browser-check --disable-sync --disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,msEdgeFirstRunExperience,EdgeSync --disable-background-networking --disable-component-update --disable-extensions --disable-default-apps --no-service-autorun --disable-client-side-phishing-detection --disable-domain-reliability --no-pings --disable-breakpad"
sh.Run """" & edge & """ --app=""" & url & """ --start-fullscreen --user-data-dir=""" & profile & """ " & edgeFlags, 1, False
