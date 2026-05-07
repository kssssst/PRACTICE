param(
    [Parameter(Mandatory = $true)]
    [string] $PayloadDir,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'

$payloadRoot = (Resolve-Path -LiteralPath $PayloadDir).Path
$files = Get-ChildItem -LiteralPath $payloadRoot -File -Recurse | Sort-Object FullName

if (-not $files) {
    throw "No files found in payload directory: $payloadRoot"
}

if (-not ($files | Where-Object { $_.Name -ieq 'TrayApp.exe' })) {
    throw "TrayApp.exe is required in the payload because the installer must install the application executable."
}

if (-not ($files | Where-Object { $_.Name -ieq 'TrayService.exe' })) {
    throw "TrayService.exe is required in the payload because the installer registers it as a Windows service."
}

function Convert-ToWixId {
    param([string] $Value)

    $id = $Value -replace '[^A-Za-z0-9_\.]', '_'
    if ($id -notmatch '^[A-Za-z_]') {
        $id = "x_$id"
    }
    if ($id.Length -gt 64) {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $hashBytes = $sha256.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Value))
            $hash = [System.BitConverter]::ToString($hashBytes).Replace('-', '').Substring(0, 12)
        }
        finally {
            $sha256.Dispose()
        }
        $id = $id.Substring(0, 50) + '_' + $hash
    }
    return $id
}

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
    return [System.Uri]::UnescapeDataString($relativeUri).Replace('\', '/').TrimStart('/')
}

function Get-RelativeDirectoryName {
    param([string] $RelativePath)

    $normalizedPath = $RelativePath.Replace('\', '/').Trim('/')
    $lastSeparator = $normalizedPath.LastIndexOf('/')
    if ($lastSeparator -lt 0) {
        return ''
    }

    return $normalizedPath.Substring(0, $lastSeparator)
}

function Get-RelativeFileName {
    param([string] $RelativePath)

    $normalizedPath = $RelativePath.Replace('\', '/').Trim('/')
    $lastSeparator = $normalizedPath.LastIndexOf('/')
    if ($lastSeparator -lt 0) {
        return $normalizedPath
    }

    return $normalizedPath.Substring($lastSeparator + 1)
}

function Convert-ToXmlAttribute {
    param([string] $Value)
    return [System.Security.SecurityElement]::Escape($Value)
}

$relativeDirectories = $files |
    ForEach-Object {
        $relativeFile = Get-RelativePayloadPath -BasePath $payloadRoot -Path $_.FullName
        $relativeDir = Get-RelativeDirectoryName $relativeFile
        while ($relativeDir) {
            $relativeDir
            $relativeDir = Get-RelativeDirectoryName $relativeDir
        }
    } |
    Where-Object { $_ } |
    Sort-Object -Unique

$directoryIds = @{}
foreach ($dir in $relativeDirectories) {
    $directoryIds[$dir] = 'dir_' + (Convert-ToWixId $dir)
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('<?xml version="1.0" encoding="utf-8"?>')
$lines.Add('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')

if ($relativeDirectories) {
    $lines.Add('  <Fragment>')
    $lines.Add('    <DirectoryRef Id="INSTALLFOLDER">')

    foreach ($dir in $relativeDirectories) {
        if ($dir -notmatch '[\\/]' ) {
            $name = Convert-ToXmlAttribute (Get-RelativeFileName $dir)
            $lines.Add("      <Directory Id=`"$($directoryIds[$dir])`" Name=`"$name`" />")
        }
    }

    $lines.Add('    </DirectoryRef>')

    foreach ($dir in $relativeDirectories) {
        $parent = Get-RelativeDirectoryName $dir
        if ($parent) {
            $parentDirectoryId = $directoryIds[$parent]
            if (-not $parentDirectoryId) {
                throw "Internal error: no WiX directory id was generated for payload directory '$parent'."
            }

            $name = Convert-ToXmlAttribute (Get-RelativeFileName $dir)
            $lines.Add("    <DirectoryRef Id=`"$parentDirectoryId`">")
            $lines.Add("      <Directory Id=`"$($directoryIds[$dir])`" Name=`"$name`" />")
            $lines.Add('    </DirectoryRef>')
        }
    }

    $lines.Add('  </Fragment>')
}

$components = New-Object System.Collections.Generic.List[string]
$filesByDirectory = $files | Group-Object {
    $relativeFile = Get-RelativePayloadPath -BasePath $payloadRoot -Path $_.FullName
    $relativeDir = Get-RelativeDirectoryName $relativeFile
    if ($relativeDir) { $relativeDir } else { '.' }
}

$lines.Add('  <Fragment>')
foreach ($group in $filesByDirectory) {
    $directoryRef = if ($group.Name -eq '.') { 'INSTALLFOLDER' } else { $directoryIds[$group.Name] }
    if (-not $directoryRef) {
        throw "Internal error: no WiX directory id was generated for payload directory '$($group.Name)'."
    }

    $lines.Add("    <DirectoryRef Id=`"$directoryRef`">")

    foreach ($file in $group.Group) {
        $relativeFile = Get-RelativePayloadPath -BasePath $payloadRoot -Path $file.FullName
        $componentId = 'cmp_' + (Convert-ToWixId $relativeFile)
        $fileId = 'fil_' + (Convert-ToWixId $relativeFile)
        $source = '$(var.PayloadDir)\' + ($relativeFile -replace '/', '\')
        $source = Convert-ToXmlAttribute $source
        $name = Convert-ToXmlAttribute (Get-RelativeFileName $relativeFile)
        $components.Add($componentId) | Out-Null

        $lines.Add("      <Component Id=`"$componentId`" Guid=`"*`">")
        $lines.Add("        <File Id=`"$fileId`" Source=`"$source`" Name=`"$name`" KeyPath=`"yes`" />")

        if ($file.Name -ieq 'TrayApp.exe') {
            $lines.Add('        <Shortcut Id="TrayAppStartMenuShortcut" Directory="ApplicationProgramsFolder" Name="TrayApp" WorkingDirectory="INSTALLFOLDER" Advertise="yes" />')
            $lines.Add('        <RemoveFolder Id="RemoveTrayAppStartMenuFolder" Directory="ApplicationProgramsFolder" On="uninstall" />')
        }

        if ($file.Name -ieq 'TrayService.exe') {
            $lines.Add('        <ServiceInstall Id="TrayServiceInstall" Name="TrayService" DisplayName="Tray Service" Description="Starts TrayApp in interactive user sessions." Type="ownProcess" Start="auto" ErrorControl="normal" Account="LocalSystem" />')
            $lines.Add('        <ServiceControl Id="TrayServiceControl" Name="TrayService" Start="install" Stop="both" Remove="uninstall" Wait="yes" />')
        }

        $lines.Add('      </Component>')
    }

    $lines.Add('    </DirectoryRef>')
}
$lines.Add('  </Fragment>')

$lines.Add('  <Fragment>')
$lines.Add('    <ComponentGroup Id="AppPayload">')
foreach ($componentId in $components) {
    $lines.Add("      <ComponentRef Id=`"$componentId`" />")
}
$lines.Add('    </ComponentGroup>')
$lines.Add('  </Fragment>')
$lines.Add('</Wix>')

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
[System.IO.File]::WriteAllLines($OutputPath, $lines, [System.Text.UTF8Encoding]::new($false))

Write-Host "Generated WiX payload file: $OutputPath"
