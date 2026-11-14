Option Explicit
Dim fso, sh, base, wmi, procs, p

Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")
base = fso.GetParentFolderName(WScript.ScriptFullName)

On Error Resume Next
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set procs = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='wscript.exe' AND CommandLine LIKE '%watch-slides.vbs%'")
For Each p In procs
  sh.Run "taskkill /PID " & p.ProcessId & " /F", 0, True
Next
Set procs = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='msedge.exe' AND (CommandLine LIKE '%html-ppt-edge-profile%' OR CommandLine LIKE '%--app=%V2.html%')")
For Each p In procs
  sh.Run "taskkill /PID " & p.ProcessId & " /F", 0, True
Next
Set procs = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='powershell.exe' AND (CommandLine LIKE '%bring-edge.ps1%' OR CommandLine LIKE '%bring-wps.ps1%')")
For Each p In procs
  sh.Run "taskkill /PID " & p.ProcessId & " /F", 0, True
Next
fso.DeleteFile base & "\watcher.lock", True
fso.DeleteFile base & "\cover-show.flag", True
fso.DeleteFile base & "\cover-hide.flag", True
fso.DeleteFile base & "\cover-exit.flag", True
Err.Clear

MsgBox "watcher and Edge app stopped. Press Esc in WPS if the slideshow is still open.", 64, "TEF Watcher"
