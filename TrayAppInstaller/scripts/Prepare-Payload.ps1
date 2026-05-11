param(
    [Parameter(Mandatory = $true)]
    [string] $AppBuildBin,

    [Parameter(Mandatory = $true)]
    [string] $PayloadDir
)

$ErrorActionPreference = 'Stop'

$resolvedBin = (Resolve-Path -LiteralPath $AppBuildBin).Path

if (Test-Path -LiteralPath $PayloadDir) {
    Remove-Item -LiteralPath $PayloadDir -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $PayloadDir | Out-Null

function Get-RelativePayloadPath {
    param(
        [string] $BasePath,
        [string] $Path
    )

    $baseFullPath = [System.IO.Path]::GetFullPath($BasePath)
    if (-not $baseFullPath.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseFullPath += [System.IO.Path]::DirectorySeparatorChar
    }

    $pathFullPath = [System.IO.Path]::GetFullPath($Path)
    $baseUri = [System.Uri]::new($baseFullPath)
    $pathUri = [System.Uri]::new($pathFullPath)
    $relativeUri = $baseUri.MakeRelativeUri($pathUri).ToString()
    return [System.Uri]::UnescapeDataString($relativeUri).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

$requiredFiles = @(
    'TrayApp.exe',
    'TrayService.exe'
)

foreach ($file in $requiredFiles) {
    $matches = @(Get-ChildItem -LiteralPath $resolvedBin -Filter $file -File -Recurse -ErrorAction SilentlyContinue)
    if (-not $matches) {
        throw "Required application artifact was not found under '$resolvedBin': $file"
    }
}

$excludedDirectories = @(
    '.git',
    '.vs',
    'CMakeFiles',
    'Testing'
)

$excludedExtensions = @(
    '.ilk',
    '.ipdb',
    '.iobj',
    '.lastbuildstate',
    '.log',
    '.obj',
    '.pch',
    '.tlog'
)

$runtimeExtensions = @(
    '.exe',
    '.dll',
    '.config',
    '.json',
    '.xml',
    '.ini',
    '.yaml',
    '.yml',
    '.txt',
    '.dat',
    '.bin',
    '.pak',
    '.rcc',
    '.qm',
    '.mui',
    '.pri',
    '.winmd',
    '.appxmanifest',
    '.manifest',
    '.local',
    '.ico',
    '.png',
    '.jpg',
    '.jpeg',
    '.bmp',
    '.gif',
    '.webp',
    '.svg',
    '.wav',
    '.mp3',
    '.mp4',
    '.ttf',
    '.otf'
)

$runtimeFiles = Get-ChildItem -LiteralPath $resolvedBin -File -Recurse -Force |
    Where-Object {
        $file = $_
        $hasExcludedParent = $false
        $relativeFile = Get-RelativePayloadPath -BasePath $resolvedBin -Path $file.FullName
        foreach ($part in ($relativeFile -split '[\\/]')) {
            if ($excludedDirectories -contains $part) {
                $hasExcludedParent = $true
                break
            }
        }

        -not $hasExcludedParent -and
        -not ($excludedExtensions -contains $file.Extension.ToLowerInvariant()) -and
        ($runtimeExtensions -contains $file.Extension.ToLowerInvariant())
    } |
    Sort-Object FullName

if (-not $runtimeFiles) {
    throw "No runtime files were found under '$resolvedBin'."
}

foreach ($file in $runtimeFiles) {
    $relativePath = Get-RelativePayloadPath -BasePath $resolvedBin -Path $file.FullName
    $destination = Join-Path $PayloadDir $relativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}

$payloadFiles = Get-ChildItem -LiteralPath $PayloadDir -File -Recurse
if (-not $payloadFiles) {
    throw "Payload directory is empty: $PayloadDir"
}

foreach ($file in $requiredFiles) {
    if (-not ($payloadFiles | Where-Object { $_.Name -ieq $file })) {
        throw "Required application artifact was not copied into the installer payload: $file"
    }
}

Write-Host "Prepared TrayApp installer payload:"
$payloadFiles | ForEach-Object {
    $relative = Resolve-Path -LiteralPath $_.FullName -Relative
    Write-Host "  $relative"
}
