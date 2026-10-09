param(
    [string]$Firmware = 'build_dpi-hdk_lb57gyd7n6_epd_hcpu',
    [string]$Toolchain = 'C:\Users\xiebo\.sifli\tools\arm-none-eabi-gcc\14.2.1\bin'
)
$ErrorActionPreference = 'Stop'
$books = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$repo = [IO.Path]::GetFullPath((Join-Path $books '../..'))
$firmwareDir = Join-Path $repo ('project/' + $Firmware)
$nm = Join-Path $Toolchain 'arm-none-eabi-nm.exe'
$strings = Join-Path $Toolchain 'arm-none-eabi-strings.exe'
$size = Join-Path $Toolchain 'arm-none-eabi-size.exe'
$exports = & $nm (Join-Path $firmwareDir 'main.elf') | ForEach-Object {
    if ($_ -match '\b__rtmsym_(\w+)$' -and $Matches[1] -notlike '*_name') { $Matches[1] }
}
if ($LASTEXITCODE) { throw 'Cannot read firmware exports' }
$header = Get-Content -LiteralPath (Join-Path $firmwareDir 'epd_app_profile.h') -Raw
if ($header -notmatch 'EPD_APP_BUILD_ID\s+"([a-f0-9]+)"') { throw 'No firmware profile' }
$profile = $Matches[1]
foreach ($name in @('mobi', 'doc')) {
    $module = Join-Path $books "codecs/$name/output/bk_$name.so"
    $undefined = @(& $nm -D --undefined-only $module | ForEach-Object { ($_ -split '\s+')[-1] })
    if ($LASTEXITCODE) { throw "Cannot read $module" }
    $defined = @(& $nm -D --defined-only $module | ForEach-Object { ($_ -split '\s+')[-1] })
    if ($defined.Count -ne 1 -or $defined[0] -ne 'book_converter') { throw "Unexpected exports: $defined" }
    $forbidden = $undefined | Where-Object {
        $_ -match '^(_impure_ptr|_global_impure_ptr|malloc|calloc|realloc|free|fopen|fclose|fread|fwrite|fseek|fprintf|vfprintf|fflush|getenv|exit|_sbrk|stdin|stdout|stderr)$' -or $_ -match '^lv_'
    }
    if ($forbidden) { throw "Forbidden imports: $forbidden" }
    $marker = @(& $strings $module | Select-String ('EPDAPP:' + $profile))
    if (!$marker.Count) { throw "Profile mismatch: $module" }
    $missing = @($undefined | Where-Object { $_ -notin $exports })
    [pscustomobject]@{
        Module = "bk_$name.so"
        FileBytes = (Get-Item -LiteralPath $module).Length
        ImportCount = $undefined.Count
        Missing = $missing -join ','
        Profile = $profile
        SHA256 = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash
    } | Format-List
    & $size $module
    if ($missing.Count) { throw "Missing firmware exports: $missing" }
}
