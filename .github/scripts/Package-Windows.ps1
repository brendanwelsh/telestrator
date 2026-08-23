[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $BuildInstaller
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version
    $DisplayName = if ( $BuildSpec.displayName ) { $BuildSpec.displayName } else { $ProductName }
    $Author = $BuildSpec.author
    $Website = $BuildSpec.website

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*-Installer.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    if ( $BuildInstaller ) {
        Log-Group "Building Windows installer for ${ProductName}..."

        $IsccPath = Get-Command 'iscc.exe' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
        if ( -not $IsccPath ) {
            $Candidates = @(
                "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
                "${Env:ProgramFiles}\Inno Setup 6\ISCC.exe"
            )
            $IsccPath = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        }
        if ( -not $IsccPath ) {
            Write-Information 'Inno Setup (ISCC.exe) not found; installing via Chocolatey...'
            Invoke-External choco install innosetup --no-progress --yes
            $IsccPath = "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        }
        if ( -not ( Test-Path $IsccPath ) ) {
            throw "Inno Setup compiler (ISCC.exe) not found - cannot build installer."
        }

        # The plugin tree cmake installed for this config: release/<config>/<name>/
        $SourceDir = "${ProjectRoot}/release/${Configuration}/${ProductName}"
        $IssArgs = @(
            "${ProjectRoot}/build-aux/installer-Windows.iss"
            "/DPluginId=${ProductName}"
            "/DAppName=${DisplayName}"
            "/DAppVersion=${ProductVersion}"
            "/DAppPublisher=${Author}"
            "/DAppURL=${Website}"
            "/DSourceDir=${SourceDir}"
            "/DOutputDir=${ProjectRoot}/release"
            "/DLicenseFile=${ProjectRoot}/LICENSE"
        )
        Invoke-External "$IsccPath" @IssArgs
        Log-Group
    }
}

Package
