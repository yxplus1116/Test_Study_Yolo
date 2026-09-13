param(
    [string]$Port = 'COM3',
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'
$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 500
$serial.WriteTimeout = 500
$serial.DtrEnable = $true
$serial.RtsEnable = $true

try {
    $serial.Open()
    Write-Host "Opened $Port at $Baud 8N1. Waiting for MAKCU startup..."
    Start-Sleep -Milliseconds 1500
    $serial.DiscardInBuffer()

    function Send-Makcu([string]$Command) {
        $serial.Write($Command + "`r`n")
        Start-Sleep -Milliseconds 200
        $reply = if ($serial.BytesToRead -gt 0) { $serial.ReadExisting() } else { '' }
        Write-Host "TX: $Command"
        if ($reply) { Write-Host ("RX: " + ($reply -replace "`r", '<CR>' -replace "`n", '<LF>')) }
        else { Write-Host 'RX: <none> (legacy firmware may not return an ACK)' }
    }

    Send-Makcu 'km.version()'
    Send-Makcu 'km.getpos()'
    Write-Host 'Moving right by about 20 HID units. Confirm USB1 is on the target PC and USB3 has a real mouse.'
    Send-Makcu 'km.move(20,0)'
    Write-Host 'Done. Press Enter to exit.'
    [void](Read-Host)
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}

