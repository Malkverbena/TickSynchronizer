# Runs prebuilt TickSynchronizer benchmark executables on a Windows test machine.
# Requires only Windows PowerShell; build tools, Git, Python, and source files are unnecessary.

param(
    [ValidateSet("single", "double", "all")]
    [string]$Precision = "double",
    [int]$Cpu = -1,
    [ValidateNotNullOrEmpty()]
    [string]$CpuClass = "desktop",
    [switch]$Quick,
    [switch]$AllowDirty,
    [switch]$ListCpus,
    [string]$OutputDir = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BenchmarkArguments
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutputDir) { $OutputDir = Join-Path $ScriptDir "benchmark_reports" }

function Assert-PackageIntegrity {
    $MetadataPath = Join-Path $ScriptDir "PACKAGE_METADATA.txt"
    if (-not (Test-Path $MetadataPath)) { return }

    $ManifestPath = Join-Path $ScriptDir "SHA256SUMS.txt"
    if (-not (Test-Path $ManifestPath)) { throw "Exported package is missing SHA256SUMS.txt" }
    foreach ($Line in (Get-Content -Encoding ASCII -Path $ManifestPath)) {
        if (-not $Line.Trim()) { continue }
        $Parts = $Line -split '\s+', 2
        if ($Parts.Count -ne 2 -or $Parts[0] -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Invalid SHA256SUMS.txt entry: $Line"
        }
        $Relative = ($Parts[1] -replace '^\./', '').Replace([char]'/', [IO.Path]::DirectorySeparatorChar)
        $Path = Join-Path $ScriptDir $Relative
        if (-not (Test-Path -Path $Path -PathType Leaf)) { throw "Package file is missing: $Relative" }
        $Actual = (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
        if ($Actual -ne $Parts[0].ToLowerInvariant()) { throw "Package hash mismatch: $Relative" }
    }
    Write-Host "TICKSYNCHRONIZER_BENCHMARK_PACKAGE_INTEGRITY_OK platform=windows"
}

function Assert-Report([string]$JsonPath, [string]$BinaryHash) {
    $Report = Get-Content -Raw -Encoding UTF8 $JsonPath | ConvertFrom-Json

    if ($Report.schema_version -ne 3) { throw "Unexpected report schema: $($Report.schema_version)" }
    if ($Report.benchmark_suite_version -ne 1) { throw "Unexpected benchmark suite: $($Report.benchmark_suite_version)" }
    if ($Report.api_version -ne 4) { throw "Unexpected public API version: $($Report.api_version)" }
    if ($Report.wire_protocol_version -ne 0 -or $Report.wire_protocol_revision -ne 2) {
        throw "Unexpected experimental wire contract"
    }
    if ($Report.build.runtime_backend -ne "windows-native") { throw "Report is not from the Windows native backend" }
    if ($Report.build.binary_sha256.ToLowerInvariant() -ne $BinaryHash) { throw "Report binary hash does not match the executable" }
    if ($Report.build.affinity_requested -ne "yes" -or $Report.build.affinity_applied -ne "yes") {
        throw "Requested CPU affinity was not applied: $($Report.build.affinity_error)"
    }
    if ($Report.build.affinity_actual_cpu -eq "unknown" -or $Report.build.affinity_error -ne "none") {
        throw "Native CPU affinity was not verified: $($Report.build.affinity_error)"
    }
    foreach ($Field in @("cpu_core", "cpu_package", "numa_node", "l3_cache_id", "thread_siblings")) {
        $TopologyValue = [string]$Report.build.PSObject.Properties[$Field].Value
        if ($TopologyValue -eq "unknown") {
            throw "Native CPU topology field is unavailable: $Field"
        }
    }
    if (-not $Quick -and -not $AllowDirty -and $Report.build.source_state -ne "clean") {
        throw "Official Windows benchmark uses a binary built from a dirty source tree"
    }
    if (-not $Quick -and -not $AllowDirty -and -not $Report.official_eligible) {
        throw "Report is not eligible for official comparison"
    }
    if ($Report.datasets.Count -ne 7) { throw "Unexpected dataset count: $($Report.datasets.Count)" }

    foreach ($Dataset in $Report.datasets) {
        if ($Dataset.integrity.round_trip_failures -ne 0) { throw "$($Dataset.name): round-trip failure" }
        if ($Dataset.integrity.determinism_failures -ne 0) { throw "$($Dataset.name): determinism failure" }
        if ([double]$Dataset.encode.checksum -eq 0) { throw "$($Dataset.name): zero encode checksum" }
        if ([double]$Dataset.decode.checksum -eq 0) { throw "$($Dataset.name): zero decode checksum" }
    }
    if ($Report.invalid_packets.accepted -ne 0 -or $Report.invalid_packets.rejected -le 0) {
        throw "Invalid-packet corpus validation failed"
    }
    if ([double]$Report.invalid_packets.decode.checksum -eq 0) {
        throw "Invalid-packet checksum is zero"
    }

    return $Report
}

function Show-Cpus {
    $System = Get-CimInstance Win32_ComputerSystem
    $Processor = Get-CimInstance Win32_Processor | Select-Object -First 1
    Write-Host "Computer: $($System.Manufacturer) $($System.Model)"
    Write-Host "CPU: $($Processor.Name)"
    Write-Host "Logical processors: $($System.NumberOfLogicalProcessors)"
    Write-Host ""
    $Binary = Join-Path $ScriptDir "tick_synchronizer_protocol_benchmark.double.exe"
    if (-not (Test-Path $Binary)) {
        $Binary = Join-Path $ScriptDir "tick_synchronizer_protocol_benchmark.single.exe"
    }
    if (-not (Test-Path $Binary)) { throw "No benchmark executable is available for native topology discovery" }
    & $Binary --list-cpus
    if ($LASTEXITCODE -ne 0) { throw "Native CPU topology discovery failed" }
    Write-Host ""
    Write-Host "Choose one primary hardware thread from each distinct L3 domain."
    Write-Host "Use hardware documentation before assigning architecture-specific CPU-class labels."
}

function Run-One([string]$SelectedPrecision) {
    $Binary = Join-Path $ScriptDir "tick_synchronizer_protocol_benchmark.$SelectedPrecision.exe"
    if (-not (Test-Path $Binary)) { throw "Benchmark binary not found: $Binary" }

    $Topology = @(& $Binary --list-cpus)
    if ($LASTEXITCODE -ne 0) { throw "Native CPU topology discovery failed: $Binary" }
    & $Binary --self-test
    if ($LASTEXITCODE -ne 0) { throw "Benchmark self-test failed: $Binary" }

    $System = Get-CimInstance Win32_ComputerSystem
    $Processor = Get-CimInstance Win32_Processor | Select-Object -First 1
    $OperatingSystem = Get-CimInstance Win32_OperatingSystem
    $Timestamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ")
    $DeviceSlug = (($System.Manufacturer + "-" + $System.Model).ToLowerInvariant() -replace '[^a-z0-9]+','-').Trim('-')
    $CpuClassSlug = ($CpuClass.ToLowerInvariant() -replace '[^a-z0-9]+','-').Trim('-')
    if (-not $CpuClassSlug) { $CpuClassSlug = "unspecified" }
    $ReportDir = Join-Path $OutputDir "$Timestamp-windows-$DeviceSlug-$CpuClassSlug-cpu$Cpu-$SelectedPrecision-suite1"
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
    $Json = Join-Path $ReportDir "results.json"
    $Csv = Join-Path $ReportDir "results.csv"
    $Log = Join-Path $ReportDir "benchmark.log"
    $Hash = (Get-FileHash -Algorithm SHA256 $Binary).Hash.ToLowerInvariant()

    $env:TICKSYNC_BENCHMARK_RUNTIME_BACKEND = "windows-native"
    $env:TICKSYNC_BENCHMARK_DEVICE_MANUFACTURER = [string]$System.Manufacturer
    $env:TICKSYNC_BENCHMARK_DEVICE_MODEL = [string]$System.Model
    $env:TICKSYNC_BENCHMARK_OS_VERSION = [string]$OperatingSystem.Caption
    $env:TICKSYNC_BENCHMARK_OS_BUILD = [string]$OperatingSystem.BuildNumber
    $env:TICKSYNC_BENCHMARK_SOC_MODEL = [string]$Processor.Name
    $env:TICKSYNC_BENCHMARK_EXECUTABLE_PATH = (Resolve-Path $Binary).Path
    $env:TICKSYNC_BENCHMARK_BINARY_SHA256 = $Hash
    $env:TICKSYNC_BENCHMARK_CPU_MODEL = [string]$Processor.Name
    $env:TICKSYNC_BENCHMARK_CPU_CLASS = $CpuClass
    foreach ($Name in @(
        "TICKSYNC_BENCHMARK_CPU_CORE",
        "TICKSYNC_BENCHMARK_CPU_PACKAGE",
        "TICKSYNC_BENCHMARK_NUMA_NODE",
        "TICKSYNC_BENCHMARK_L3_CACHE_ID",
        "TICKSYNC_BENCHMARK_THREAD_SIBLINGS"
    )) {
        Remove-Item "Env:$Name" -ErrorAction SilentlyContinue
    }
    $env:TICKSYNC_BENCHMARK_SCALING_DRIVER = "windows-power-management"
    $env:TICKSYNC_BENCHMARK_SCALING_GOVERNOR = "unknown"
    $env:TICKSYNC_BENCHMARK_CPU_MIN_FREQUENCY_KHZ = "unknown"
    $env:TICKSYNC_BENCHMARK_CPU_MAX_FREQUENCY_KHZ = ([int64]$Processor.MaxClockSpeed * 1000).ToString()

    $Arguments = @("--json", $Json, "--csv", $Csv, "--cpu", $Cpu.ToString())
    if ($Quick) { $Arguments += "--quick" }
    if ($BenchmarkArguments) { $Arguments += $BenchmarkArguments }

    & $Binary @Arguments 2>&1 | Tee-Object -FilePath $Log
    if ($LASTEXITCODE -ne 0) { throw "Windows benchmark execution failed" }

    $Report = Assert-Report $Json $Hash

    $EnvironmentLines = @(
        "Generated UTC: $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))",
        "Computer: $($System.Manufacturer) $($System.Model)",
        "Windows: $($OperatingSystem.Caption) build $($OperatingSystem.BuildNumber)",
        "CPU: $($Processor.Name)",
        "Logical processors: $($System.NumberOfLogicalProcessors)",
        "Selected CPU: $Cpu",
        "CPU class: $($Report.build.cpu_class)",
        "Applied CPU: $($Report.build.affinity_actual_cpu)",
        "Processor group: $($Report.build.processor_group)",
        "Core: $($Report.build.cpu_core)",
        "Package: $($Report.build.cpu_package)",
        "NUMA node: $($Report.build.numa_node)",
        "L3 cache id: $($Report.build.l3_cache_id)",
        "Thread siblings: $($Report.build.thread_siblings)",
        "Precision: $SelectedPrecision",
        "Binary: $Binary",
        "Binary SHA-256: $Hash",
        "Module commit embedded at cross-build: $($Report.build.module_commit)",
        "Source state embedded at cross-build: $($Report.build.source_state)",
        "Compiler: $($Report.build.compiler)",
        "Compiler path: $($Report.build.compiler_path)",
        "Compiler flags: $($Report.build.compiler_flags)",
        "Official eligible: $($Report.official_eligible)"
    )
    $EnvironmentLines += ""
    $EnvironmentLines += "Native CPU topology:"
    $EnvironmentLines += $Topology
    $EnvironmentLines | Set-Content -Encoding UTF8 (Join-Path $ReportDir "environment.txt")

    Get-ChildItem -File $ReportDir | Where-Object Name -ne "SHA256SUMS.txt" | Sort-Object Name | ForEach-Object {
        $FileHash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
        "$FileHash  $($_.Name)"
    } | Set-Content -Encoding ASCII (Join-Path $ReportDir "SHA256SUMS.txt")

    Write-Host "TICKSYNCHRONIZER_WINDOWS_BENCHMARK_REPORT_OK precision=$SelectedPrecision cpu=$Cpu official=$($Report.official_eligible) report=$ReportDir"
}

Assert-PackageIntegrity

if ($ListCpus) {
    Show-Cpus
    exit 0
}
if ($Cpu -lt 0) { throw "-Cpu N is required so the native executable can verify affinity" }

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if ($Precision -eq "all") {
    Run-One "double"
    Run-One "single"
} else {
    Run-One $Precision
}
