Option Explicit
Dim edgeFlags

Dim fso, sh, base, deck, html, edge, url, temp, profile, cmd
Dim app, pres, targetSlide, state, pos, count, testMode, edgeAt, resumeAt
Dim lock, lastLock, maxEdgeWait, attempt, age
Dim wmi, procs, p

Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")
base = fso.GetParentFolderName(WScript.ScriptFullName)
deck = base & "\LyMtrix-TEF-Lv6-live.pptx"
html = base & "\V2.html"
targetSlide = 13
maxEdgeWait = 600
testMode = WScript.Arguments.Named.Exists("test")

Sub Log(msg)
  On Error Resume Next
  Dim f
  Set f = fso.OpenTextFile(base & "\watch-log.txt", 8, True)
  If Err.Number = 0 Then
    f.WriteLine Now & " " & msg
    f.Close
  End If
  Err.Clear
End Sub

Sub DeleteLock()
  On Error Resume Next
  fso.DeleteFile lock, True
  Err.Clear
End Sub

Sub TouchLock()
  If Timer - lastLock > 5 Then
    On Error Resume Next
    fso.CreateTextFile(lock, True).WriteLine Now
    Err.Clear
    lastLock = Timer
  End If
End Sub

Sub Fail(msg)
  Log "FAIL: " & msg
  MsgBox msg, 16, "TEF Watcher"
  DeleteLock
  WScript.Quit 1
End Sub

Function FindEdge()
  Dim candidate
  candidate = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
  If fso.FileExists(candidate) Then
    FindEdge = candidate
    Exit Function
  End If
  candidate = "C:\Program Files\Microsoft\Edge\Application\msedge.exe"
  If fso.FileExists(candidate) Then
    FindEdge = candidate
    Exit Function
  End If
  FindEdge = "msedge.exe"
End Function

Function OpenDeck(path)
  Dim tries, candidate
  Set OpenDeck = Nothing
  For tries = 1 To 3
    On Error Resume Next
    Set candidate = app.Presentations.Open(path, True, False, True)
    If Err.Number = 0 And Not candidate Is Nothing Then
      Set OpenDeck = candidate
      Exit Function
    End If
    Err.Clear
    WScript.Sleep 1000
  Next
End Function

Function StartShow(p)
  Dim tries, waited
  StartShow = False
  For tries = 1 To 3
    On Error Resume Next
    p.SlideShowSettings.StartingSlide = 1
    p.SlideShowSettings.ShowWithAnimation = True
    p.SlideShowSettings.Run()
    Err.Clear
    waited = 0
    Do While waited < 2500
      WScript.Sleep 100
      waited = waited + 100
      On Error Resume Next
      If app.SlideShowWindows.Count > 0 Then
        Err.Clear
        WScript.Sleep 150
        StartShow = True
        Exit Function
      End If
      Err.Clear
    Loop
    WScript.Sleep 300
  Next
End Function

Function ExitShow()
  Dim tries
  ExitShow = False
  For tries = 1 To 3
    On Error Resume Next
    app.SlideShowWindows.Item(1).View.Exit()
    WScript.Sleep 300
    If Err.Number = 0 Then
      ExitShow = True
      Exit Function
    End If
    Err.Clear
    WScript.Sleep 300
  Next
End Function

Function JumpTo(slideNo)
  Dim tries
  JumpTo = False
  For tries = 1 To 3
    On Error Resume Next
    app.SlideShowWindows.Item(1).View.GotoSlide slideNo
    WScript.Sleep 300
    If Err.Number = 0 Then
      JumpTo = True
      Exit Function
    End If
    Err.Clear
    WScript.Sleep 300
  Next
End Function

Function EdgeOpen()
  Dim query, items
  EdgeOpen = True
  On Error Resume Next
  Set wmi = GetObject("winmgmts:\\.\root\cimv2")
  Set items = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='msedge.exe' AND CommandLine LIKE '%--app=%V2.html%'")
  If Err.Number = 0 Then
    EdgeOpen = (items.Count > 0)
  End If
  Err.Clear
End Function

Sub LaunchEdge()
  On Error Resume Next
  sh.Run cmd, 1, False
  If Err.Number <> 0 Then Log "edge launch error: " & Err.Description
  Err.Clear
  If fso.FileExists(base & "\bring-edge.ps1") Then
    sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & base & "\bring-edge.ps1""", 0, False
  End If
  WScript.Sleep 1200
End Sub

Sub FocusShow(tag, waitSeconds)
  Dim helper, rc
  TouchLock
  On Error Resume Next
  ' WPS exposes SlideShowWindow.Activate; use it first because it is instant.
  app.SlideShowWindows.Item(1).Activate
  Err.Clear
  helper = base & "\bring-wps.ps1"
  If fso.FileExists(helper) Then
    ' The helper unlocks the Windows foreground lock with AttachThreadInput
    ' and re-activates the WPS fullscreen slideshow window.
    rc = sh.Run("powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & helper & """ -TimeoutSeconds " & waitSeconds, 0, True)
    TouchLock
    If Err.Number <> 0 Then
      Log "focus helper error (" & tag & "): " & Err.Description
      Err.Clear
    Else
      Log "focus helper " & tag & " rc=" & rc
    End If
  Else
    Log "focus helper missing: " & helper
  End If
End Sub

Sub ResumeShow()
  If Not StartShow(pres) Then
    Log "resume failed: could not restart show"
    state = 0
    Exit Sub
  End If
  FocusShow "resume", 6
  If Not JumpTo(targetSlide) Then
    Log "goto target failed"
  Else
    WScript.Sleep 500
    On Error Resume Next
    pos = app.SlideShowWindows.Item(1).View.CurrentShowPosition
    If Err.Number = 0 Then Log "resume post-goto pos=" & pos
    Err.Clear
  End If
  resumeAt = Timer
  state = 2
  Log "resumed at slide " & targetSlide
End Sub

' ---- startup preflight ----
If Not fso.FileExists(deck) Then Fail "Deck not found: " & deck
If Not fso.FileExists(html) Then Fail "V2.html not found: " & html
edge = FindEdge()
If edge = "" Then Fail "Microsoft Edge not found"
url = "file:///" & Replace(html, "\", "/")
temp = fso.GetSpecialFolder(2)
profile = temp & "\html-ppt-edge-profile"
edgeFlags = "--no-first-run --no-default-browser-check --disable-sync --disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,msEdgeFirstRunExperience,EdgeSync --disable-background-networking --disable-component-update --disable-extensions --disable-default-apps --no-service-autorun --disable-client-side-phishing-detection --disable-domain-reliability --no-pings --disable-breakpad"
cmd = """" & edge & """ --app=""" & url & """ --start-fullscreen --user-data-dir=""" & profile & """ " & edgeFlags

' ---- single instance, heartbeat lock ----
lock = base & "\watcher.lock"
If fso.FileExists(lock) Then
  On Error Resume Next
  age = DateDiff("s", fso.GetFile(lock).DateLastModified, Now)
  If Err.Number = 0 And age >= 0 And age < 15 Then
    MsgBox "watch-slides.vbs is already running.", 48, "TEF Watcher"
    WScript.Quit 1
  End If
  Err.Clear
  fso.DeleteFile lock, True
End If
On Error Resume Next
fso.CreateTextFile(lock, True).WriteLine Now
Err.Clear
lastLock = Timer

' ---- start WPS ----
Set app = Nothing
For attempt = 1 To 3
  On Error Resume Next
  Set app = CreateObject("Kwpp.Application")
  If Err.Number = 0 And Not app Is Nothing Then Exit For
  Err.Clear
  WScript.Sleep 1000
Next
If app Is Nothing Then Fail "Could not start WPS (Kwpp.Application)."
On Error Resume Next
app.Visible = True
Err.Clear
Log "WPS started"

Set pres = OpenDeck(deck)
If pres Is Nothing Then
  DeleteLock
  Fail "Could not open deck. Close any open copy and retry: " & deck
End If
Log "deck opened"

If Not StartShow(pres) Then
  DeleteLock
  Fail "Could not start the WPS slideshow. Close other presentations and retry."
End If
Log "show started"
FocusShow "start", 3

If testMode Then
  WScript.Sleep 1500
  If Not JumpTo(targetSlide) Then Log "test goto failed"
  Log "test goto " & targetSlide
End If

state = 0
edgeAt = 0
resumeAt = 0

Do
  On Error Resume Next
  count = app.SlideShowWindows.Count
  If Err.Number <> 0 Then
    Log "WPS closed or COM unavailable; exiting"
    DeleteLock
    WScript.Quit
  End If

  If state = 0 Then
    If count > 0 Then
      pos = app.SlideShowWindows.Item(1).View.CurrentShowPosition
      If Err.Number = 0 And pos = targetSlide Then
        Log "at target slide; exiting show"
        If Not ExitShow() Then Log "exit show failed"
        WScript.Sleep 400
        LaunchEdge
        edgeAt = Timer
        state = 1
        Log "edge launched"
      End If
      Err.Clear
    End If
  ElseIf state = 1 Then
    If Timer - edgeAt > maxEdgeWait Then
      Log "edge close timeout; resuming"
      ResumeShow
    ElseIf Timer - edgeAt > 2 Then
      If Not EdgeOpen() Then
        Log "edge closed; resuming"
        ResumeShow
      End If
    End If
    Err.Clear
  ElseIf state = 2 Then
    If Timer - resumeAt > 1.5 Then
      If count = 0 Then
        Log "show ended; rearming"
        state = 0
      Else
        pos = app.SlideShowWindows.Item(1).View.CurrentShowPosition
        If Err.Number = 0 And pos <> targetSlide Then
          Log "left target slide (" & pos & "); rearming"
          state = 0
        End If
        Err.Clear
      End If
    End If
  End If

  TouchLock
  Err.Clear
  WScript.Sleep 300
Loop