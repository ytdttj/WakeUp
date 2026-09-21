Set ws = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

exe = "D:\WorkBuddyProjects\WakeUp\bin\WakeGuardC.exe"

If Not fso.FileExists(exe) Then
  MsgBox "WakeGuardC.exe not found:" & vbCrLf & exe, 16, "WakeGuard"
  WScript.Quit 1
End If

ws.CurrentDirectory = "D:\WorkBuddyProjects\WakeUp\bin"
ws.Run """" & exe & """", 0, False
