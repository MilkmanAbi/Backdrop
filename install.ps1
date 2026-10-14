# Backdrop one-line installer. In PowerShell:
#     irm https://raw.githubusercontent.com/MilkmanAbi/Backdrop/main/install.ps1 | iex
# Downloads the latest Backdrop-Setup-*.exe from this repo's GitHub releases, checks its SHA-256 against the one
# GitHub published for the upload, installs it silently for the current user (no admin needed) and starts it.
& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'   # Windows PowerShell's progress bar makes downloads crawl
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $repo = 'MilkmanAbi/Backdrop'

    Write-Host ''
    Write-Host '  Backdrop - video wallpaper + colour filters' -ForegroundColor Cyan
    Write-Host ''

    if ([Environment]::Is64BitOperatingSystem -eq $false) { throw 'Backdrop needs 64-bit Windows 10 (1809) or newer.' }

    # Backdrop installs for one Windows user. An admin window that belongs to a *different* account (a typed-in admin
    # password) would install it for that account instead of yours.
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $desk = Get-CimInstance Win32_Process -Filter "Name='explorer.exe' AND SessionId=$((Get-Process -Id $PID).SessionId)" -ErrorAction SilentlyContinue |
        Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner -ErrorAction SilentlyContinue
    if ($desk -and $desk.User -and ("$($desk.Domain)\$($desk.User)" -ne $me)) {
        throw "This PowerShell is running as $me, but you're signed in as $($desk.Domain)\$($desk.User). Open a normal PowerShell (no admin needed) and run the command again."
    }

    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'Backdrop-installer' }
    $asset = $rel.assets | Where-Object { $_.name -like 'Backdrop-Setup-*.exe' } | Select-Object -First 1
    if (-not $asset) { throw "The latest release ($($rel.tag_name)) has no installer attached." }

    $file = Join-Path $env:TEMP $asset.name
    Write-Host ("  Downloading {0} ({1:N1} MB)" -f $asset.name, ($asset.size / 1MB))
    Write-Host "  from $($rel.html_url)"
    Invoke-WebRequest $asset.browser_download_url -OutFile $file -UseBasicParsing

    # GitHub records a SHA-256 for every release upload: a corrupted or swapped file never gets run
    if ($asset.digest -like 'sha256:*') {
        $want = $asset.digest.Substring(7).ToUpperInvariant()
        $got = (Get-FileHash $file -Algorithm SHA256).Hash
        if ($got -ne $want) { Remove-Item $file -ErrorAction SilentlyContinue; throw 'Checksum mismatch - the download is damaged or not the official file. Nothing was installed.' }
        Write-Host "  Checksum OK  (SHA-256 $got)" -ForegroundColor DarkGray
    }

    # Downloaded this way the file carries no "from the internet" mark, so Windows doesn't show the
    # "Windows protected your PC" screen for it (that screen appears for unsigned apps downloaded in a browser).
    Unblock-File $file

    Write-Host '  Installing (per-user, no admin needed)...'
    $p = Start-Process $file -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/nolaunch=1' -PassThru -Wait
    Remove-Item $file -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "Setup failed (exit code $($p.ExitCode))." }

    # start it exactly the way Windows will at every sign-in: as you, never elevated (even from an admin window)
    $task = Get-ScheduledTask -TaskName "Backdrop ($env:USERNAME)" -ErrorAction SilentlyContinue
    if ($task) { Start-ScheduledTask -InputObject $task }
    else { Start-Process (Join-Path $env:LOCALAPPDATA 'Programs\Backdrop\backdrop.exe') }

    Write-Host ''
    Write-Host "  Backdrop $($rel.tag_name) is installed and running." -ForegroundColor Green
    Write-Host '  Look for its icon next to the clock. It starts by itself every time you sign in.'
    Write-Host '  Uninstall any time: tray icon > Uninstall Backdrop, or Settings > Apps.'
    Write-Host ''
}
