# Script para ativar ESP-IDF no PowerShell
# Execute: .\activate_esp_idf.ps1

param(
    [string]$IdfPath
)

$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$found = $false

function Invoke-ExportScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath
    )

    $exportScript = Join-Path $BasePath "export.ps1"
    if (-not (Test-Path $exportScript)) {
        return $false
    }

    Write-Host "ESP-IDF encontrado em: $BasePath" -ForegroundColor Green
    Write-Host "Ativando ambiente..." -ForegroundColor Yellow

    # Dot-source para manter variaveis/funcoes no shell atual.
    . $exportScript
    return $true
}

Write-Host "=== Procurando ESP-IDF ===" -ForegroundColor Cyan
Write-Host "Diretorio do script: $scriptDir" -ForegroundColor Gray
Write-Host ""

$candidatePaths = @(
    $IdfPath,
    $env:IDF_PATH,
    (Join-Path $scriptDir "esp-idf"),
    (Join-Path $scriptDir "tools\esp-idf"),
    "C:\Espressif\frameworks\esp-idf-v5.5.1",
    "C:\Espressif\frameworks\esp-idf-v5.5.0",
    "C:\Espressif\frameworks\esp-idf-v5.4",
    "C:\Espressif\frameworks\esp-idf",
    "$env:USERPROFILE\esp\esp-idf",
    "$env:USERPROFILE\esp-idf",
    "C:\esp\esp-idf",
    "$env:LOCALAPPDATA\esp-idf",
    "$env:ProgramFiles\Espressif\esp-idf",
    "$env:ProgramFiles(x86)\Espressif\esp-idf"
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique

Write-Host "Procurando em caminhos conhecidos..." -ForegroundColor Yellow
foreach ($path in $candidatePaths) {
    if (Test-Path $path) {
        if (Invoke-ExportScript -BasePath $path) {
            $found = $true
            break
        }
    }
}

if (-not $found) {
    Write-Host "Procurando em subdiretorios do projeto e do usuario..." -ForegroundColor Yellow
    $searchRoots = @($scriptDir, "$env:USERPROFILE", "$env:LOCALAPPDATA") | Select-Object -Unique

    foreach ($root in $searchRoots) {
        if (-not (Test-Path $root)) {
            continue
        }

        try {
            $foundExport = Get-ChildItem -Path $root -Filter "export.ps1" -Recurse -Depth 4 -ErrorAction SilentlyContinue |
                Where-Object { $_.Directory.Name -eq "esp-idf" } |
                Select-Object -First 1

            if ($foundExport) {
                if (Invoke-ExportScript -BasePath $foundExport.Directory.FullName) {
                    $found = $true
                    break
                }
            }
        } catch {
            # Ignora erros de permissao/acesso
        }
    }
}

if (-not $found) {
    Write-Host ""
    Write-Host "ESP-IDF nao encontrado automaticamente." -ForegroundColor Red
    Write-Host "Informe o caminho da pasta esp-idf (ou Enter para cancelar)." -ForegroundColor Yellow
    $manualPath = Read-Host "Caminho"

    if (-not [string]::IsNullOrWhiteSpace($manualPath)) {
        if (Test-Path $manualPath) {
            $found = Invoke-ExportScript -BasePath $manualPath
            if (-not $found) {
                Write-Host "export.ps1 nao encontrado em: $manualPath" -ForegroundColor Red
            }
        } else {
            Write-Host "Caminho nao encontrado: $manualPath" -ForegroundColor Red
        }
    }
}

if (-not $found) {
    exit 1
}

Write-Host ""
Write-Host "Ambiente ESP-IDF ativado." -ForegroundColor Green
if ($env:IDF_PATH) {
    Write-Host "IDF_PATH: $env:IDF_PATH" -ForegroundColor Gray
}
Write-Host "Comandos sugeridos:" -ForegroundColor Cyan
Write-Host "  idf.py build" -ForegroundColor White
Write-Host "  idf.py flash monitor" -ForegroundColor White
Write-Host ""
