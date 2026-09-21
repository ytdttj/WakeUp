Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set col = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name = 'WakeGuardC.exe'")
count = 0
For Each p In col
  p.Terminate
  count = count + 1
Next
If count = 0 Then
  MsgBox "WakeGuard is not running.", 64, "WakeGuard"
End If
