# Deploy ZBrush UDIM Baker to Maxon ZBrush 2026 ZStartup
param (
    [string]$ZBrushPath = "C:\Program Files\Maxon ZBrush 2026"
)

$ErrorActionPreference = "Stop"

$ZPlugsDir = Join-Path $ZBrushPath "ZStartup\ZPlugs64"
$DataDir = Join-Path $ZPlugsDir "UDIMBakerData"
$BinDir = "$PSScriptRoot\build\bin\Release"

Write-Host "== Deploying ZBrush UDIM Baker ==" -ForegroundColor Cyan
Write-Host "Target Plugin Folder: $ZPlugsDir"
Write-Host "Target Data Folder:   $DataDir"

if (-not (Test-Path $ZPlugsDir)) {
    throw "ZBrush ZPlugs64 directory not found at $ZPlugsDir"
}

# 1. Create UDIMBakerData directory if not exists
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

# 2. Copy ZScript to ZPlugs64
Copy-Item "$PSScriptRoot\zscript\UDIMBaker.txt" -Destination $ZPlugsDir -Force
Write-Host "Copied UDIMBaker.txt to $ZPlugsDir" -ForegroundColor Green

# 3. Copy Binaries to UDIMBakerData
Copy-Item "$BinDir\UDIMBakerBridge.dll" -Destination $DataDir -Force
Copy-Item "$BinDir\UDIMBakerApp.exe" -Destination $DataDir -Force
Copy-Item "$BinDir\embree3.dll" -Destination $DataDir -Force
Copy-Item "$BinDir\tbb12.dll" -Destination $DataDir -Force

Write-Host "Copied UDIMBakerBridge.dll, UDIMBakerApp.exe, and dependencies to $DataDir" -ForegroundColor Green
Write-Host "Deployment completed successfully! Open ZBrush 2026 -> ZPlugin:UDIM Baker to use." -ForegroundColor Cyan
