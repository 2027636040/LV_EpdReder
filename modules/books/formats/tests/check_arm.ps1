param(
    [string]$Compiler = 'C:\Users\xiebo\.sifli\tools\arm-none-eabi-gcc\14.2.1\bin\arm-none-eabi-gcc.exe'
)
$ErrorActionPreference = 'Stop'
$formatRoot = Split-Path $PSScriptRoot -Parent
$projectRoot = [IO.Path]::GetFullPath((Join-Path $formatRoot '../../..'))
$armBuild = Join-Path $PSScriptRoot 'build/arm'
New-Item -ItemType Directory -Path $armBuild -Force | Out-Null
$listing = & python -c 'import json,runpy,sys; d=runpy.run_path(sys.argv[1]); print(json.dumps(d["source_files"](sys.argv[2])))' (Join-Path $formatRoot 'sources.py') $formatRoot | ConvertFrom-Json
$includeDirs = @(
    $formatRoot, (Join-Path $formatRoot '../third_party/libxml2'),
    (Join-Path $formatRoot '../third_party/libxml2/include'),
    (Join-Path $projectRoot 'src'),
    (Join-Path $projectRoot 'project/build_dpi-hdk_lb57gyd7n6_epd_hcpu'),
    (Join-Path $projectRoot 'SiFli-SDK/rtos/rtthread/include'),
    (Join-Path $projectRoot 'SiFli-SDK/rtos/rtthread/components/dfs/include'),
    (Join-Path $projectRoot 'SiFli-SDK/rtos/rtthread/components/drivers/include'),
    (Join-Path $projectRoot 'SiFli-SDK/rtos/rtthread/components/drivers/audio'),
    (Join-Path $projectRoot 'SiFli-SDK/rtos/rtthread/components/finsh'),
    (Join-Path $projectRoot 'SiFli-SDK/rtos/rtthread/components/libc/compilers/newlib')
)
$options = @('-std=gnu99', '-mcpu=cortex-m33', '-mthumb', '-DLIBXML_STATIC',
    '-DBOOK_FORMAT_TARGET', '-Os', '-fPIC', '-ffunction-sections', '-fdata-sections', '-fstack-usage')
foreach ($directory in $includeDirs) { $options += '-I' + $directory }
$objects = @()
foreach ($source in $listing) {
    $vendor = $source.Contains('third_party')
    $prefix = if ($vendor) { 'vendor_' } else { '' }
    $object = Join-Path $armBuild ($prefix + [IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    $warnings = if ($vendor) { @('-Wno-error') } else { @('-Wall', '-Wextra', '-Werror') }
    $compileArgs = @($options) + @($warnings) + @('-c', $source, '-o', $object)
    & $Compiler @compileArgs
    if ($LASTEXITCODE) { throw "Compilation failed: $source" }
    $objects += $object
}
$nm = Join-Path (Split-Path $Compiler) 'arm-none-eabi-nm.exe'
$defined = @{}
& $nm --defined-only --extern-only @objects | ForEach-Object {
    if ($_ -match '\s[ABCDGRSTVW]\s+(\S+)$') { $defined[$Matches[1]] = $true }
}
$undefined = & $nm -u @objects | ForEach-Object {
    if ($_ -match '\sU\s+(\S+)$' -and !$defined.ContainsKey($Matches[1])) { $Matches[1] }
} | Sort-Object -Unique
$forbidden = $undefined | Where-Object { $_ -match '^(_impure_ptr|malloc|calloc|realloc|free|fopen|fclose|fseek|fread|fwrite|fflush|fprintf|vfprintf|getenv|sscanf)$' }
if ($forbidden) { throw ('Forbidden imports: ' + ($forbidden -join ', ')) }
'ARM compile and forbidden-import audit passed.'
'Imports: ' + ($undefined -join ', ')
