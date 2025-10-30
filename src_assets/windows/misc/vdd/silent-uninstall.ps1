# Run this script in a powershell with administrator rights (run as administrator)
[CmdletBinding()]
param(

);

# Create temp directory
$tempDir = $PWD;
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null;
# Define path to devcon executable
$devconExe = Join-Path $tempDir "devcon.exe";
& $devconExe remove "Root\MttVDD";
Write-Host "Driver installation removed." -ForegroundColor Green;


