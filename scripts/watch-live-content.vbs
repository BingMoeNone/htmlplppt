' watch-live-content.vbs - space-triggered live HTML on existing content slides.
' Keep this file, stop-live.vbs, focus-agent.ps1, black-fade.ps1, bring-wps.ps1,
' focus-lib.ps1, the deck and the HTML pages in one folder. Double-click once;
' allow the first security prompt. On a configured slide, press Space once to
' launch its HTML through a black fade. In the HTML, Space advances and closes
' on the last step; Esc starts the return fade anytime. After returning, Space
' continues to the next slide; the same slide will not trigger again in one show.
' WPS COM transient errors are retried; a final failure cleans up Edge, the
' focus agent, the cover and flags. Use /test to jump to the first target slide.

Option Explicit

Dim fso, sh, base, deck, deckName, stage, temp, edge, edgeFlags
Dim app, pres, state, pos, prevSlide, count, maxEdgeWait
Dim targetSlides, targetHtmls, targetProfiles, targetLabels, doneFlags()
Dim srcNames, srcPath, dstPath, i, idx
Dim wmi, procs, lock, lastLock, age, attempt
Dim exitFlag, edgeDeadline, currentTarget, showEndedLogged, ignoreUntil, testMode
Dim coverFadeFlag, coverReadyFlag, coverDoneFlag, coverStopFlag, coverHelper
Dim agentHelper, agentArmFlag, agentStopFlag, coverFadeWaitMs
Dim returnRequestFlag, returnPrepared, comFailCount, ownsLock

Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")
base = fso.GetParentFolderName(WScript.ScriptFullName)
' ============================================================
' CONFIG - edit for each deck. Keep all arrays the same length.
' For non-ASCII file names, build them with ChrW. Example: a
' two-character Chinese file name can be built as
'   ChrW(&H5BA1) & ChrW(&H8BA1) & ".html"
' targetHtmls are the ASCII names copied into %TEMP%\tef-live-stage.
' ============================================================
deckName = "deck-live.pptx"
targetSlides = Array(5, 6, 8)
srcNames = Array("page1.html", "page2.html", "page3.html")
targetHtmls = Array("page1.html", "page2.html", "page3.html")
targetProfiles = Array("tef-live-page1", "tef-live-page2", "tef-live-page3")
targetLabels = Array("Page1", "Page2", "Page3")
' ============================================================

deck = base & "\" & deckName
temp = fso.GetSpecialFolder(2)
stage = temp & "\tef-live-stage"
exitFlag = base & "\edge-closed.flag"
coverFadeFlag = base & "\cover-fade.flag"
coverReadyFlag = base & "\cover-ready.flag"
coverDoneFlag = base & "\cover-done.flag"
coverStopFlag = base & "\cover-stop.flag"
coverHelper = base & "\black-fade.ps1"
agentHelper = base & "\focus-agent.ps1"
agentArmFlag = base & "\agent-arm.flag"
agentStopFlag = base & "\agent-stop.flag"
returnRequestFlag = base & "\return-request.flag"
coverFadeWaitMs = 350
edgeFlags = "--no-first-run --no-default-browser-check --disable-sync --disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,msEdgeFirstRunExperience,EdgeSync --disable-background-networking --disable-component-update --disable-extensions --disable-default-apps --no-service-autorun --disable-client-side-phishing-detection --disable-domain-reliability --no-pings --disable-breakpad"
returnPrepared = False
ownsLock = False
maxEdgeWait = 600
testMode = WScript.Arguments.Named.Exists("test")

If UBound(targetSlides) <> UBound(srcNames) Or UBound(targetSlides) <> UBound(targetHtmls) Or UBound(targetSlides) <> UBound(targetProfiles) Or UBound(targetSlides) <> UBound(targetLabels) Then
  Fail "CONFIG arrays must have the same length"
End If
ReDim doneFlags(UBound(targetSlides))
For i = 0 To UBound(doneFlags)
  doneFlags(i) = False
Next

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
  If ownsLock Then
    Cleanup
  End If
  MsgBox msg, 16, "TEF Live"
  If ownsLock Then DeleteLock
  WScript.Quit 1
End Sub

Sub Cleanup()
  Dim w, p, q
  On Error Resume Next
  Log "cleanup start"
  StopAgent
  StopCover
  Set w = GetObject("winmgmts:\\.\root\cimv2")
  Set p = w.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='msedge.exe' AND CommandLine LIKE '%tef-live-%'")
  For Each q In p
    sh.Run "taskkill /PID " & q.ProcessId & " /F", 0, True
  Next
  Set p = w.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='powershell.exe' AND (CommandLine LIKE '%focus-agent.ps1%' OR CommandLine LIKE '%black-fade.ps1%' OR CommandLine LIKE '%bring-wps.ps1%' OR CommandLine LIKE '%bring-edge.ps1%')")
  For Each q In p
    sh.Run "taskkill /PID " & q.ProcessId & " /F", 0, True
  Next
  fso.DeleteFile lock, True
  fso.DeleteFile exitFlag, True
  fso.DeleteFile coverFadeFlag, True
  fso.DeleteFile coverReadyFlag, True
  fso.DeleteFile coverDoneFlag, True
  fso.DeleteFile coverStopFlag, True
  fso.DeleteFile base & "\cover-hide.flag", True
  fso.DeleteFile agentArmFlag, True
  fso.DeleteFile agentStopFlag, True
  fso.DeleteFile returnRequestFlag, True
  Err.Clear
  Log "cleanup done"
End Sub

Sub StartAgent()
  If Not fso.FileExists(agentHelper) Then
    Log "focus-agent.ps1 missing; focus may be slow"
    Exit Sub
  End If
  On Error Resume Next
  fso.DeleteFile agentArmFlag, True
  fso.DeleteFile agentStopFlag, True
  Err.Clear
  sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & agentHelper & """", 0, False
  If Err.Number <> 0 Then Log "agent launch error: " & Err.Description
  Err.Clear
  Log "agent started"
End Sub

Sub StopAgent()
  On Error Resume Next
  fso.CreateTextFile(agentStopFlag, True).WriteLine Now
  Err.Clear
End Sub

Sub ArmAgent(idx)
  On Error Resume Next
  returnPrepared = False
  fso.DeleteFile agentArmFlag, True
  fso.DeleteFile returnRequestFlag, True
  fso.CreateTextFile(agentArmFlag, True).WriteLine targetProfiles(idx) & "|" & exitFlag & "|" & coverReadyFlag
  Err.Clear
  Log "agent armed: " & targetProfiles(idx)
End Sub

Sub TriggerCover()
  On Error Resume Next
  fso.DeleteFile coverReadyFlag, True
  fso.DeleteFile coverDoneFlag, True
  fso.CreateTextFile(coverFadeFlag, True).WriteLine Now
  Err.Clear
  Log "cover fade requested"
End Sub

Sub MarkCoverReady()
  On Error Resume Next
  fso.CreateTextFile(coverReadyFlag, True).WriteLine Now
  Err.Clear
  Log "cover ready"
End Sub

Sub StopCover()
  On Error Resume Next
  fso.CreateTextFile(coverStopFlag, True).WriteLine Now
  Err.Clear
End Sub

Sub StartCover()
  If Not fso.FileExists(coverHelper) Then
    Log "black-fade.ps1 missing; no fade transition"
    Exit Sub
  End If
  On Error Resume Next
  fso.DeleteFile coverFadeFlag, True
  fso.DeleteFile coverReadyFlag, True
  fso.DeleteFile coverDoneFlag, True
  fso.DeleteFile coverStopFlag, True
  Err.Clear
  sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & coverHelper & """", 0, False
  If Err.Number <> 0 Then Log "cover launch error: " & Err.Description
  Err.Clear
  Log "cover started"
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

Sub FocusShow(tag, waitSeconds)
  Dim h, rc
  TouchLock
  On Error Resume Next
  app.SlideShowWindows.Item(1).Activate
  Err.Clear
  h = base & "\bring-wps.ps1"
  If fso.FileExists(h) Then
    rc = sh.Run("powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & h & """ -TimeoutSeconds " & waitSeconds, 0, True)
    TouchLock
    If Err.Number <> 0 Then
      Log "focus helper error (" & tag & "): " & Err.Description
      Err.Clear
    Else
      Log "focus helper " & tag & " rc=" & rc
    End If
  Else
    Log "focus helper missing: " & h
  End If
End Sub

Function TargetIndex(slideNo)
  Dim k
  TargetIndex = -1
  For k = 0 To UBound(targetSlides)
    If targetSlides(k) = slideNo Then
      TargetIndex = k
      Exit Function
    End If
  Next
End Function

Function IsDone(slideNo)
  Dim k
  IsDone = False
  k = TargetIndex(slideNo)
  If k >= 0 Then IsDone = doneFlags(k)
End Function

Sub SetDone(slideNo)
  Dim k
  k = TargetIndex(slideNo)
  If k >= 0 Then doneFlags(k) = True
End Sub

Sub ResetDone()
  Dim k
  For k = 0 To UBound(doneFlags)
    doneFlags(k) = False
  Next
End Sub

Sub LaunchEdge(idx)
  Dim htmlPath, profile, cmd
  htmlPath = stage & "\" & targetHtmls(idx)
  profile = temp & "\" & targetProfiles(idx)
  cmd = """" & edge & """ --app=""" & "file:///" & Replace(htmlPath, "\", "/") & """ --start-fullscreen --user-data-dir=""" & profile & """ " & edgeFlags
  ArmAgent idx
  On Error Resume Next
  sh.Run cmd, 1, False
  If Err.Number <> 0 Then Log "edge launch error: " & Err.Description
  Err.Clear
  Log "edge launched: " & targetLabels(idx)
End Sub

Sub ResumeShow(target)
  If Not StartShow(pres) Then
    Log "resume failed: could not restart show"
    state = 0
    Exit Sub
  End If
  FocusShow "resume", 6
  If Not JumpTo(target) Then
    Log "goto target failed"
  Else
    Log "resumed at slide " & target
  End If
  prevSlide = -1
  ignoreUntil = DateAdd("s", 2, Now)
  state = 0
End Sub

' ---- preflight ----
If Not fso.FileExists(deck) Then Fail "Deck not found: " & deck
edge = FindEdge()
If edge = "" Then Fail "Microsoft Edge not found"
If Not fso.FolderExists(stage) Then
  On Error Resume Next
  fso.CreateFolder(stage)
  If Err.Number <> 0 Then
    Log "stage folder error: " & Err.Description
    Err.Clear
    Fail "Could not create stage folder: " & stage
  End If
  Err.Clear
End If
For i = 0 To UBound(targetHtmls)
  srcPath = base & "\" & srcNames(i)
  dstPath = stage & "\" & targetHtmls(i)
  If Not fso.FileExists(srcPath) Then Fail "HTML not found: " & srcPath
  On Error Resume Next
  fso.CopyFile srcPath, dstPath, True
  If Err.Number <> 0 Then
    Log "stage copy error: " & Err.Description
    Err.Clear
    Fail "Could not stage HTML: " & srcNames(i)
  End If
  Err.Clear
Next

' ---- single instance, heartbeat lock ----
lock = base & "\watcher.lock"
If fso.FileExists(lock) Then
  On Error Resume Next
  age = DateDiff("s", fso.GetFile(lock).DateLastModified, Now)
  If Err.Number = 0 And age >= 0 And age < 15 Then
    MsgBox "watch-live.vbs is already running.", 48, "TEF Live"
    WScript.Quit 1
  End If
  Err.Clear
  fso.DeleteFile lock, True
End If
On Error Resume Next
fso.CreateTextFile(lock, True).WriteLine Now
Err.Clear
lastLock = Timer
ownsLock = True

StartCover
StartAgent

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

On Error Resume Next
pos = app.SlideShowWindows.Item(1).View.CurrentShowPosition
If Err.Number = 0 Then prevSlide = pos Else prevSlide = 1
Err.Clear
ignoreUntil = DateAdd("s", 1, Now)
state = 0
showEndedLogged = False
edgeDeadline = Now

If testMode Then
  If JumpTo(targetSlides(0)) Then
    prevSlide = targetSlides(0)
    Log "test jump to slide " & targetSlides(0)
  Else
    Log "test jump failed"
  End If
  ignoreUntil = DateAdd("s", 1, Now)
End If

' ---- monitor ----
comFailCount = 0
Do
  On Error Resume Next
  count = app.SlideShowWindows.Count
  If Err.Number <> 0 Then
    comFailCount = comFailCount + 1
    Log "COM busy: err=" & Err.Number & " " & Err.Description & " count=" & comFailCount
    Err.Clear
    If comFailCount >= 20 Then
      Log "WPS COM unavailable after retries; exiting"
      Cleanup
      WScript.Quit
    End If
  Else
    comFailCount = 0
    Err.Clear

    If state = 1 Then
    If Not returnPrepared Then
      If fso.FileExists(returnRequestFlag) Then
        fso.DeleteFile returnRequestFlag, True
        Log "return requested by page"
        TriggerCover
        WScript.Sleep coverFadeWaitMs
        ResumeShow currentTarget
        state = 1
        returnPrepared = True
      End If
    End If
    If fso.FileExists(exitFlag) Then
      fso.DeleteFile exitFlag, True
      If returnPrepared Then
        Log "edge closed after return; cover fade out"
        MarkCoverReady
        returnPrepared = False
      Else
        Log "edge closed; resuming"
        TriggerCover
        WScript.Sleep coverFadeWaitMs
        ResumeShow currentTarget
        MarkCoverReady
      End If
      state = 0
    ElseIf Now > edgeDeadline Then
      If returnPrepared Then
        Log "edge close timeout after return; cover fade out"
        MarkCoverReady
        returnPrepared = False
      Else
        Log "edge close timeout; resuming"
        TriggerCover
        WScript.Sleep coverFadeWaitMs
        ResumeShow currentTarget
        MarkCoverReady
      End If
      state = 0
    End If
  Else
    If count > 0 Then
      pos = app.SlideShowWindows.Item(1).View.CurrentShowPosition
      If Err.Number = 0 Then
        If Now < ignoreUntil Then
          prevSlide = pos
        ElseIf prevSlide < 0 Then
          prevSlide = pos
        ElseIf pos <> prevSlide Then
          If pos > prevSlide And TargetIndex(prevSlide) >= 0 And Not IsDone(prevSlide) Then
            currentTarget = prevSlide
            idx = TargetIndex(currentTarget)
            SetDone currentTarget
            Log "trigger slide " & currentTarget & " -> " & targetLabels(idx)
            TriggerCover
            WScript.Sleep coverFadeWaitMs
            On Error Resume Next
            fso.DeleteFile exitFlag, True
            Err.Clear
            LaunchEdge idx
            If Not ExitShow() Then Log "exit show failed"
            edgeDeadline = DateAdd("s", maxEdgeWait, Now)
            state = 1
          End If
          prevSlide = pos
        End If
        showEndedLogged = False
      End If
      Err.Clear
    Else
      If Not showEndedLogged Then
        Log "show ended; waiting for restart"
        showEndedLogged = True
        ResetDone
        prevSlide = -1
      End If
    End If
  End If
  End If ' COM error check

  TouchLock
  Err.Clear
  WScript.Sleep 100
Loop
