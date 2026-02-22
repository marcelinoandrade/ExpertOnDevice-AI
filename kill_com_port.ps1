# Script para matar processos que estão usando a porta COM (similar ao kill no Linux)
# Uso: .\kill_com_port.ps1 COM11
#      .\kill_com_port.ps1 11

param(
    [Parameter(Mandatory = $true)]
    [string]$ComPort
)

$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }

# Remove "COM" (case-insensitive) se o usuario digitou "COM11" em vez de "11"
$ComPort = ($ComPort.Trim()) -replace '(?i)^COM', ''

if ($ComPort -notmatch '^\d+$') {
    Write-Host "Valor de porta invalido: $ComPort. Use o numero da porta (ex: 11) ou COM11." -ForegroundColor Red
    exit 1
}

Write-Host "Buscando processos que podem estar usando COM${ComPort}..." -ForegroundColor Cyan
Write-Host "Diretorio do script: $scriptDir" -ForegroundColor Gray

# Lista de processos PROTEGIDOS (nunca devem ser finalizados)
$protectedProcesses = @("cursor", "code", "vscode", "devenv", "notepad++", "notepad")

# Lista expandida de processos que geralmente usam portas seriais
$processNames = @(
    "python", "python3", "python3.9", "python3.10", "python3.11", "python3.12",
    "serial-discovery", "idf.py", "monitor", "esptool", "esptool.py",
    "putty", "teraterm", "ttermpro", "hyperterminal",
    "arduino", "arduino_debug", "arduino-builder",
    "node", "nodejs"  # Node.js pode ter processos de serial
)

$foundProcesses = @()

# Busca processos por nome
foreach ($procName in $processNames) {
    $procs = Get-Process -Name $procName -ErrorAction SilentlyContinue
    if ($procs) {
        $foundProcesses += $procs
    }
}

# Também tenta encontrar processos que podem ter "COM" no título da janela ou linha de comando
try {
    $comPortPattern = "COM${ComPort}"
    $allProcs = Get-Process | Where-Object {
        $_.MainWindowTitle -like "*${comPortPattern}*" -or
        $_.MainWindowTitle -like "*Serial*" -or
        $_.CommandLine -like "*${comPortPattern}*" -or
        $_.CommandLine -like "*serial*"
    } -ErrorAction SilentlyContinue

    if ($allProcs) {
        $foundProcesses += $allProcs
    }
} catch {
    # Ignora erros ao acessar CommandLine (pode requerer privilégios)
}

# Remove duplicatas
$foundProcesses = $foundProcesses | Sort-Object -Unique -Property Id

# Remove processos protegidos (IDEs e editores não devem ser finalizados)
$foundProcesses = $foundProcesses | Where-Object {
    $procName = $_.ProcessName.ToLower()
    $protectedProcesses -notcontains $procName
}

if ($foundProcesses) {
    Write-Host "`nProcessos encontrados que podem estar usando COM${ComPort}:" -ForegroundColor Yellow
    $foundProcesses | Select-Object ProcessName, Id, @{Name='Memory(MB)';Expression={[math]::Round($_.WS/1MB,2)}} | Format-Table -AutoSize
    Write-Host "Nota: IDEs e editores (Cursor, VS Code, etc.) foram excluídos da lista por segurança." -ForegroundColor Cyan
    
    Write-Host "`nDeseja finalizar estes processos? (S/N): " -NoNewline -ForegroundColor Yellow
    $response = Read-Host
    
    if ($response -eq 'S' -or $response -eq 's' -or $response -eq 'Y' -or $response -eq 'y') {
        Write-Host "`nFinalizando processos..." -ForegroundColor Red
        $foundProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
    
    Start-Sleep -Seconds 1
    
        # Verifica se foram finalizados
        $stillRunning = $foundProcesses | Where-Object { 
            Get-Process -Id $_.Id -ErrorAction SilentlyContinue 
        }
        
        if ($stillRunning) {
            Write-Host "Aviso: Alguns processos ainda estão em execução. Tente executar como Administrador." -ForegroundColor Yellow
        } else {
            Write-Host "Processos finalizados com sucesso!" -ForegroundColor Green
            Write-Host "A porta COM${ComPort} deve estar livre agora." -ForegroundColor Green
        }
    } else {
        Write-Host "Operação cancelada." -ForegroundColor Gray
    }
} else {
    Write-Host "Nenhum processo relacionado encontrado." -ForegroundColor Gray
    Write-Host "A porta COM${ComPort} pode estar livre ou sendo usada por outro processo." -ForegroundColor Gray
    Write-Host "`nDica: Se a porta ainda estiver em uso, tente:" -ForegroundColor Cyan
    Write-Host "  1. Fechar todas as janelas de terminal/IDE" -ForegroundColor Cyan
    Write-Host "  2. Executar este script como Administrador" -ForegroundColor Cyan
    Write-Host "  3. Reiniciar o computador se necessário" -ForegroundColor Cyan
}
