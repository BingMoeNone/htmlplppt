# Shared Win32 window lookup / force-foreground helpers.
# Dot-source from bring-edge.ps1 and bring-wps.ps1. Keep this file ASCII-only.
$ErrorActionPreference = 'SilentlyContinue'

if (-not ('TefWindow' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class TefWindow {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }

  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hWnd, int nIndex);
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr hWnd, bool fAltTab);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

  // WPS slideshow window titles normally contain this CJK word. Built from
  // code points so this file stays pure ASCII for Windows PowerShell 5.1.
  public static readonly string WpsTitleWord =
    new string(new char[] { (char)0x5E7B, (char)0x706F, (char)0x7247 });

  public const int GWL_STYLE = -16;
  public const int WS_POPUP = unchecked((int)0x80000000);
  public const int WS_MINIMIZEBOX = 0x00020000;
  public const int WS_MAXIMIZEBOX = 0x00010000;
  const int SW_RESTORE = 9;
  const byte VK_MENU = 0x12;
  const uint KEYEVENTF_KEYUP = 0x0002;

  public static string TextOf(IntPtr hWnd) {
    StringBuilder sb = new StringBuilder(512);
    GetWindowText(hWnd, sb, sb.Capacity);
    return sb.ToString();
  }

  public static string ClassOf(IntPtr hWnd) {
    StringBuilder sb = new StringBuilder(256);
    GetClassName(hWnd, sb, sb.Capacity);
    return sb.ToString();
  }

  // Largest visible top-level window owned by pid. Edge app launches briefly
  // create a small splash window; picking the largest avoids focusing it.
  public static IntPtr FindMainWindowByPid(uint pid) {
    IntPtr best = IntPtr.Zero;
    long bestArea = -1;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p;
      GetWindowThreadProcessId(h, out p);
      if (p != pid || !IsWindowVisible(h)) return true;
      RECT r;
      if (!GetWindowRect(h, out r)) return true;
      long area = (long)(r.Right - r.Left) * (long)(r.Bottom - r.Top);
      if (area > bestArea) { bestArea = area; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }

  public static IntPtr FindByPid(uint pid, string titlePart, string classEquals) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p;
      GetWindowThreadProcessId(h, out p);
      if (p != pid || !IsWindowVisible(h)) return true;
      string t = TextOf(h);
      string c = ClassOf(h);
      bool titleOk = String.IsNullOrEmpty(titlePart) || t.IndexOf(titlePart, StringComparison.OrdinalIgnoreCase) >= 0;
      bool classOk = String.IsNullOrEmpty(classEquals) || c.Equals(classEquals, StringComparison.OrdinalIgnoreCase);
      if (titleOk && classOk) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  // Pick the best fullscreen-like window owned by one of pids.
  // Scores WPS slideshow traits: known class names, title, WS_POPUP, no min/max boxes.
  public static IntPtr FindFullscreenForPids(uint[] pids, int minW, int minH) {
    if (pids == null || pids.Length == 0) return IntPtr.Zero;
    HashSet<uint> set = new HashSet<uint>(pids);
    IntPtr best = IntPtr.Zero;
    int bestScore = -1;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint pid;
      GetWindowThreadProcessId(h, out pid);
      if (!set.Contains(pid) || !IsWindowVisible(h)) return true;
      RECT r;
      if (!GetWindowRect(h, out r)) return true;
      int w = r.Right - r.Left;
      int ht = r.Bottom - r.Top;
      if (w < minW || ht < minH) return true;
      string cls = ClassOf(h);
      string title = TextOf(h);
      int style = GetWindowLong(h, GWL_STYLE);
      int score = 0;
      if (cls.Equals("screenClass", StringComparison.OrdinalIgnoreCase) ||
          cls.Equals("SlideShowClass", StringComparison.OrdinalIgnoreCase) ||
          cls.Equals("Widget", StringComparison.OrdinalIgnoreCase)) score += 2;
      // U+5E7B U+706F U+7247 is the CJK word used by WPS slideshow titles.
      if (title.IndexOf(WpsTitleWord, StringComparison.OrdinalIgnoreCase) >= 0 ||
          title.IndexOf("Slide Show", StringComparison.OrdinalIgnoreCase) >= 0 ||
          title.IndexOf("Slideshow", StringComparison.OrdinalIgnoreCase) >= 0) score += 3;
      if ((style & WS_POPUP) != 0) score += 2;
      if ((style & WS_MINIMIZEBOX) == 0 && (style & WS_MAXIMIZEBOX) == 0) score += 2;
      if (score > bestScore) { bestScore = score; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }

  // Fallback lookup by title. Rank: the CJK slideshow word (0), then the
  // caller-supplied parts in order. Lower rank wins, so a show window title
  // beats an editor window titled with the deck name.
  public static IntPtr FindTitleForPids(uint[] pids, string[] titleParts) {
    if (pids == null || pids.Length == 0 || titleParts == null) return IntPtr.Zero;
    HashSet<uint> set = new HashSet<uint>(pids);
    IntPtr found = IntPtr.Zero;
    int foundRank = int.MaxValue;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint pid;
      GetWindowThreadProcessId(h, out pid);
      if (!set.Contains(pid) || !IsWindowVisible(h)) return true;
      string t = TextOf(h);
      int rank = int.MaxValue;
      if (t.IndexOf(WpsTitleWord, StringComparison.OrdinalIgnoreCase) >= 0) rank = 0;
      for (int i = 0; i < titleParts.Length; i++) {
        if (!String.IsNullOrEmpty(titleParts[i]) &&
            t.IndexOf(titleParts[i], StringComparison.OrdinalIgnoreCase) >= 0) {
          int r = i + 1;
          if (r < rank) rank = r;
        }
      }
      if (rank < foundRank) { foundRank = rank; found = h; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  public static bool IsForeground(IntPtr hWnd) {
    return hWnd != IntPtr.Zero && IsWindowVisible(hWnd) && GetForegroundWindow() == hWnd;
  }

  // True when the window carries a WPS/PowerPoint slideshow trait. Used when
  // scanning wps.exe (which may also own Writer windows) as a fallback.
  public static bool LooksLikeShow(IntPtr hWnd) {
    if (hWnd == IntPtr.Zero) return false;
    string t = TextOf(hWnd);
    string c = ClassOf(hWnd);
    if (t.IndexOf(WpsTitleWord, StringComparison.OrdinalIgnoreCase) >= 0) return true;
    if (t.IndexOf("Slide Show", StringComparison.OrdinalIgnoreCase) >= 0 ||
        t.IndexOf("Slideshow", StringComparison.OrdinalIgnoreCase) >= 0) return true;
    if (c.Equals("screenClass", StringComparison.OrdinalIgnoreCase) ||
        c.Equals("SlideShowClass", StringComparison.OrdinalIgnoreCase)) return true;
    return false;
  }

  // Force hWnd to the foreground. Handles the Windows foreground lock with
  // AttachThreadInput + a synthetic Alt tap, then verifies the result.
  public static bool Force(IntPtr hWnd, bool topMost) {
    if (hWnd == IntPtr.Zero || !IsWindow(hWnd)) return false;
    if (topMost) {
      SetWindowPos(hWnd, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040);
    }
    if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
    if (IsForeground(hWnd)) return true;

    SetForegroundWindow(hWnd);
    if (IsForeground(hWnd)) return true;

    SwitchToThisWindow(hWnd, true);
    if (IsForeground(hWnd)) return true;

    uint fgPid;
    uint targetPid;
    uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), out fgPid);
    uint targetThread = GetWindowThreadProcessId(hWnd, out targetPid);
    uint curThread = GetCurrentThreadId();
    bool attachedFg = fgThread != 0 && fgThread != curThread && AttachThreadInput(curThread, fgThread, true);
    bool attachedTarget = targetThread != 0 && targetThread != curThread &&
                          targetThread != fgThread && AttachThreadInput(curThread, targetThread, true);
    BringWindowToTop(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    if (attachedTarget) AttachThreadInput(curThread, targetThread, false);
    if (attachedFg) AttachThreadInput(curThread, fgThread, false);
    if (IsForeground(hWnd)) return true;

    // Last resort: hold Alt while activating. The synthetic Alt keypress
    // unlocks the foreground lock on Windows 10/11; release it afterwards.
    keybd_event(VK_MENU, 0, 0, UIntPtr.Zero);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    bool okAlt = IsForeground(hWnd);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    if (okAlt) return true;

    SetForegroundWindow(hWnd);
    return IsForeground(hWnd);
  }
}
"@
}

function Write-TefLog([string]$Message) {
  try {
    $log = Join-Path $PSScriptRoot 'bring-log.txt'
    ("{0:o} {1}" -f (Get-Date), $Message) | Out-File -FilePath $log -Append -Encoding utf8
  } catch {}
}

function Find-TefEdgeWindow {
  $procs = @(Get-Process msedge -ErrorAction SilentlyContinue)
  foreach ($proc in $procs) {
    try {
      if ($proc.MainWindowTitle -like '*TEF-LIVE-STAGE*' -and $proc.MainWindowHandle -ne 0) {
        return [IntPtr]$proc.MainWindowHandle
      }
    } catch {}
  }
  foreach ($proc in $procs) {
    try {
      if ($proc.MainWindowTitle -like '*TEF-LIVE-STAGE*') {
        $h = [TefWindow]::FindByPid([uint32]$proc.Id, 'TEF-LIVE-STAGE', $null)
        if ($h -ne [IntPtr]::Zero) { return $h }
      }
    } catch {}
  }
  return [TefWindow]::FindWindow($null, 'TEF-LIVE-STAGE')
}

function Find-TefEdgeWindowByProfile([string]$Marker) {
  if ([string]::IsNullOrEmpty($Marker)) { return [IntPtr]::Zero }
  $procs = Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" -ErrorAction SilentlyContinue | Where-Object {
    try { $_.CommandLine -like "*$Marker*" } catch { $false }
  }
  foreach ($p in $procs) {
    $h = [TefWindow]::FindMainWindowByPid([uint32]$p.ProcessId)
    if ($h -ne [IntPtr]::Zero) { return $h }
  }
  return [IntPtr]::Zero
}
