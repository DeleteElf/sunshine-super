# Run this script in a powershell with administrator rights (run as administrator)
[CmdletBinding()]
param(
    # SHA256 hash of the DevCon binary to install
    # Possible values can be found at:
    # https://github.com/Drawbackz/DevCon-Installer/blob/master/devcon_sources.json
    # Look for the "sha256" field in the JSON for valid hash values
    [Parameter(Mandatory=$true)]
    [string]$DevconHash,
    
    # Latest stable version of VDD driver only
    [Parameter(Mandatory=$false)]
    [string]$DriverURL = "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip"
);

# Create temp directory
$tempDir = $PWD;
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null;
# Define path to devcon executable
$devconExe = Join-Path $tempDir "devcon.exe";
# Download and run DevCon Installer
if (-not (Test-Path $devconExe))
{
    $devconPath = Join-Path $tempDir "Devcon.Installer.exe";
    if (-not (Test-Path $devconPath))
    {
        Write-Host "Downloading DevCon..." -ForegroundColor Cyan;
        Invoke-WebRequest -Uri "https://github.com/Drawbackz/DevCon-Installer/releases/download/1.4-rc/Devcon.Installer.exe" -OutFile $devconPath;
    }
    Write-Host "Installing DevCon..." -ForegroundColor Cyan;
    Start-Process -FilePath $devconPath -ArgumentList "install -hash $DevconHash -update -dir `"$tempDir`"" -Wait -NoNewWindow;
    Write-Host "Installing DevCon Completed..." -ForegroundColor Cyan;
}

# Check if VDD is installed. Or else, install it
$check = & $devconExe find "Root\MttVDD";
if ($check -match "1 matching device\(s\) found") {
    Write-Host "Virtual Display Driver already present. No installation." -ForegroundColor Green;
} else {
    # Extract the signPath certificates
    $catFile = Join-Path $tempDir 'VirtualDisplayDriver\mttvdd.cat';
    if (-not (Test-Path $catFile)){
        # Download and unzip VDD
        $driverZipPath = Join-Path $tempDir 'driver.zip';
        if (-not (Test-Path $driverZipPath))
        {
            Write-Host "Downloading VDD..." -ForegroundColor Cyan;
            Invoke-WebRequest -Uri $DriverURL -OutFile $driverZipPath;
        }
        Expand-Archive -Path $driverZipPath -DestinationPath $tempDir -Force;
    }

    $signature = Get-AuthenticodeSignature -FilePath $catFile;
    $catBytes = [System.IO.File]::ReadAllBytes($catFile);
    $certificates = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2Collection;
    $certificates.Import($catBytes);

    # Create the temp directory for certificates
    $certsFolder = Join-Path $tempDir "ExportedCerts";
    if (-not (Test-Path $certsFolder))
    {
        New-Item -ItemType Directory -Path $certsFolder;
    }
    # Write and store the driver certificates on local machine
    Write-Host "Installing driver certificates on local machine." -ForegroundColor Cyan;
    foreach ($cert in $certificates) {
        $certFilePath = Join-Path -Path $certsFolder -ChildPath "$($cert.Thumbprint).cer";
        $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert) | Set-Content -Path $certFilePath -Encoding Byte;
        Import-Certificate -FilePath $certFilePath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher";
    }

    # Install VDD
    Push-Location $tempDir;
    & $devconExe install .\VirtualDisplayDriver\MttVDD.inf "Root\MttVDD";
    Pop-Location;

    Write-Host "Driver installation completed." -ForegroundColor Green;
}
Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue;

