$ErrorActionPreference = 'Stop'

$toolchainRoot = if ($env:MSYSTEM_PREFIX) {
    $env:MSYSTEM_PREFIX
} else {
    'C:\msys64\ucrt64'
}
$toolchainBin = Join-Path $toolchainRoot 'bin'

function Get-ToolPath {
    param([Parameter(Mandatory = $true)][string]$Name)

    $candidate = Join-Path $toolchainBin "$Name.exe"
    if (Test-Path -LiteralPath $candidate) {
        return $candidate
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    return $null
}

function Get-VersionLine {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$CommandArgs
    )

    $path = Get-ToolPath -Name $Name
    if (-not $path) {
        return $null
    }

    try {
        $output = & $path @CommandArgs 2>&1
        if ($LASTEXITCODE -ne 0) {
            return $null
        }
        return [string]($output | Select-Object -First 1)
    } catch {
        return $null
    }
}

$detected = [ordered]@{
    gcc = Get-VersionLine -Name 'gcc' -CommandArgs @('--version')
    'g++' = Get-VersionLine -Name 'g++' -CommandArgs @('--version')
    cmake = Get-VersionLine -Name 'cmake' -CommandArgs @('--version')
    ninja = Get-VersionLine -Name 'ninja' -CommandArgs @('--version')
    'pkg-config' = Get-VersionLine -Name 'pkg-config' -CommandArgs @('--version')
    'pkg:asio' = Get-VersionLine -Name 'pkg-config' -CommandArgs @('--modversion', 'asio')
    'pkg:libavcodec' = Get-VersionLine -Name 'pkg-config' -CommandArgs @('--modversion', 'libavcodec')
    'pkg:sdl3' = Get-VersionLine -Name 'pkg-config' -CommandArgs @('--modversion', 'sdl3')
}

$capabilities = [ordered]@{}
foreach ($entry in $detected.GetEnumerator()) {
    $capabilities[$entry.Key] = [ordered]@{
        status = if ($null -eq $entry.Value) { 'missing' } else { 'available' }
        version = $entry.Value
    }
}

$report = [ordered]@{
    schema_version = 1
    read_only = $true
    platform = [ordered]@{
        system = 'Windows'
        release = [System.Environment]::OSVersion.Version.ToString()
        architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        toolchain = 'MSYS2 UCRT64'
        toolchain_root = $toolchainRoot
    }
    capabilities = $capabilities
    next = [ordered]@{
        install = 'pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-asio mingw-w64-ucrt-x86_64-cppwinrt mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-sdl3'
        configure = 'cmake --preset debug'
        build = 'cmake --build --preset debug --target stream-server stream-client'
        test = 'deferred by current milestone'
    }
}

$report | ConvertTo-Json -Depth 6
