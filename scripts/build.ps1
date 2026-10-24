param([string]$Apk = $env:UU_REFERENCE_APK, [int]$Jobs = 6,
      [ValidateSet('ON','OFF')][string]$MediaDispatch='ON', [switch]$PrivatePackage)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    if (-not $env:DEVKITPRO) {
        $localSdk = Join-Path $projectRoot '.tools/sdk/opt/devkitpro'
        if (-not (Test-Path (Join-Path $localSdk 'devkitA64/bin/aarch64-none-elf-gcc.exe'))) {
            throw 'Set DEVKITPRO or run python scripts/bootstrap_sdk.py first.'
        }
        $env:DEVKITPRO = $localSdk.Replace('\', '/')
    }
    python scripts/fetch_deps.py
    if ($LASTEXITCODE) { throw 'Dependency download failed' }
    python scripts/patch_deps.py
    if ($LASTEXITCODE) { throw 'Dependency patch failed' }
    if ($Apk) {
        python scripts/extract_key.py $Apk
        if ($LASTEXITCODE) { throw 'APK extraction failed' }
    } elseif (-not (Test-Path -LiteralPath 'romfs/signing.key')) {
        throw 'Provide -Apk with your UU TV 3.5.0 APK path.'
    }
    $cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmakeExe) {
        $bundledCmake = Get-ChildItem (Join-Path $projectRoot '.tools') -Filter 'cmake.exe' -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($bundledCmake) { $cmakeExe = $bundledCmake.FullName }
    }
    if (-not $cmakeExe) { throw 'CMake not found: install CMake 3.21+ or place it under .tools/.' }
    $configure = @('-S', '.', '-B', 'build-nx', '-G', 'Ninja', '-DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake', '-DCMAKE_BUILD_TYPE=Release')
    $version=[regex]::Match((Get-Content CMakeLists.txt -Raw),'project\(UURemote-ns VERSION ([\d.]+)').Groups[1].Value
    if(!$version){throw 'Cannot determine source version'}
    $configure += @("-DUU_BUILD_LABEL=$version","-DUU_MEDIA_DISPATCH=$MediaDispatch",'-DUU_SRTP_MBEDTLS=ON',
        '-DUU_RECOVERY_FOCUS=OFF','-DUU_SWITCH_BATCH_RX=OFF','-DUU_RECEIVE_TIMESTAMPS=OFF',
        '-DUU_TWCC_FOUNDATION=OFF','-DUU_TWCC_OBSERVE_CLIENT=OFF','-DUU_TWCC_SEND_EXPERIMENT=OFF')
    $localNinja = Join-Path $projectRoot '.tools/ninja/ninja.exe'
    if (Test-Path -LiteralPath $localNinja) { $configure += "-DCMAKE_MAKE_PROGRAM=$($localNinja.Replace('\','/'))" }
    & $cmakeExe @configure
    if ($LASTEXITCODE) { throw 'CMake configuration failed' }
    & $cmakeExe --build build-nx -j $Jobs
    if ($LASTEXITCODE) { throw 'Switch build failed' }
    if($PrivatePackage){
        python scripts/package.py --allow-private-signing-material
        if ($LASTEXITCODE) { throw 'Packaging failed' }
    }else{
        Write-Output 'Local NRO built. It embeds locally supplied authentication material; no distribution ZIP generated.'
    }
} finally { Pop-Location }
