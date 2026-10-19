# Run the same SQL/expected-file regressions with a real private PostgreSQL
# server. An already restricted Windows process keeps its existing token;
# ordinary Windows callers use PostgreSQL's regression driver unchanged.
param(
    [Parameter(Mandatory)][string]$PgRegress,
    [Parameter(Mandatory)][string]$PgBin,
    [Parameter(Mandatory)][string]$InputRoot,
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][string]$Config
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LaplaceRegressionToken {
    [DllImport("advapi32.dll", SetLastError=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsTokenRestricted(IntPtr token);
}
'@
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not [LaplaceRegressionToken]::IsTokenRestricted($identity.Token)) {
    & $PgRegress "--bindir=$PgBin" "--inputdir=$InputRoot" "--outputdir=$BuildRoot/regress-output" `
        "--temp-instance=$BuildRoot/regress-instance" "--temp-config=$Config" `
        --no-locale --encoding=UTF8 --load-extension=postgis --load-extension=laplace mask_upgrade mask
    exit $LASTEXITCODE
}
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or
    $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::PowerUser)) {
    throw 'The private regression server requires a non-administrator token.'
}

$build = [IO.Path]::GetFullPath($BuildRoot).TrimEnd('\', '/')
$instance = [IO.Path]::GetFullPath((Join-Path $build 'regress-inherited-instance'))
if (-not $instance.StartsWith($build + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The private cluster must remain inside the build directory.'
}
if (Test-Path -LiteralPath $instance) { throw "Private cluster already exists: $instance" }
$output = Join-Path $build 'regress-output'
[void](New-Item -ItemType Directory -Path (Join-Path $output 'log') -Force)
[void](New-Item -ItemType Directory -Path (Join-Path $output 'results') -Force)
$env:PG_RESTRICT_EXEC = '1' # The token was verified above; child processes inherit it.
$server = $null
try {
    & (Join-Path $PgBin 'initdb.exe') -D $instance -A trust --no-locale --encoding=UTF8 --no-sync `
        *> (Join-Path $output 'log/initdb.log')
    if ($LASTEXITCODE -ne 0) { throw 'Private PostgreSQL initialization failed.' }
    Add-Content -LiteralPath (Join-Path $instance 'postgresql.conf') -Value ([IO.File]::ReadAllText($Config))
    # Reserve a loopback port selected by Windows, then release it for PostgreSQL.
    $portLease = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $portLease.Start()
    $port = $portLease.LocalEndpoint.Port
    $portLease.Stop()
    $server = Start-Process -FilePath (Join-Path $PgBin 'postgres.exe') -WindowStyle Hidden -PassThru `
        -ArgumentList @('-D', ('"' + $instance + '"'), '-p', $port, '-h', '127.0.0.1', '-F') `
        -RedirectStandardOutput (Join-Path $output 'log/postmaster.stdout.log') `
        -RedirectStandardError (Join-Path $output 'log/postmaster.log')
    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        & (Join-Path $PgBin 'pg_isready.exe') -h 127.0.0.1 -p $port -d postgres -q
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
        if ($server.HasExited) { throw 'Private PostgreSQL server exited during startup.' }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $ready) { throw 'Private PostgreSQL server did not become ready.' }
    $connection = @('-X', '-q', '-h', '127.0.0.1', '-p', "$port", '-U', $identity.Name.Split('\')[-1])
    & (Join-Path $PgBin 'createdb.exe') -h 127.0.0.1 -p $port -U $identity.Name.Split('\')[-1] regression
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create the private regression database.' }
    & (Join-Path $PgBin 'psql.exe') @connection -d regression -v ON_ERROR_STOP=1 `
        -c 'CREATE EXTENSION postgis; CREATE EXTENSION laplace;'
    if ($LASTEXITCODE -ne 0) { throw 'Candidate extension installation failed.' }
    foreach ($test in @('mask_upgrade', 'mask')) {
        $actual = Join-Path $output "results/$test.out"
        & (Join-Path $PgBin 'psql.exe') @connection -d regression -a -v ON_ERROR_STOP=1 `
            -f (Join-Path $InputRoot "sql/$test.sql") *> $actual
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $actual; throw "$test SQL regression failed." }
        $expected = [IO.File]::ReadAllText((Join-Path $InputRoot "expected/$test.out")).Replace("`r`n", "`n").TrimEnd()
        $observed = [IO.File]::ReadAllText($actual).Replace("`r`n", "`n").TrimEnd()
        if ($expected -cne $observed) { Get-Content -LiteralPath $actual; throw "$test output differs from its expected result." }
        Write-Output "${test}: real PostgreSQL SQL/expected-file regression passed"
    }
} finally {
    if ($server -and -not $server.HasExited) {
        & (Join-Path $PgBin 'pg_ctl.exe') -D $instance -m immediate -w stop *> (Join-Path $output 'log/stop.log')
        if ($LASTEXITCODE -ne 0) { throw 'Private PostgreSQL shutdown failed; cluster retained for inspection.' }
    }
    if (Test-Path -LiteralPath $instance) { Remove-Item -LiteralPath $instance -Recurse -Force }
}
