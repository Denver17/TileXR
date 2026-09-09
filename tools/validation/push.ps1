# 将当前 HEAD（含子模块的 Git 原始字节）同步至首台 Linux 服务器。
# 部署配置 validation.local.sh / hostfile 在刷新时保留。
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Server,
    [Parameter(Mandatory = $true)][string]$RemotePath,
    [string]$SshUser = "",
    [string]$Python = "python"
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ($Server -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw "Server must be a hostname, IPv4 address or SSH alias" }
if ($SshUser -and $SshUser -notmatch '^[A-Za-z0-9_][A-Za-z0-9_.-]*$') { throw "Invalid SshUser" }
if (-not $RemotePath.StartsWith('/') -or $RemotePath -match '[\r\n\x00]') { throw "RemotePath must be an absolute Linux directory" }
function Quote-Bash([string]$Value) {
    $quotedApostrophe = "'" + '"' + "'" + '"' + "'"
    return "'" + $Value.Replace("'", $quotedApostrophe) + "'"
}
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$destination = $Server
if ($SshUser) { $destination = "$SshUser@$Server" }
$runId = [Guid]::NewGuid().ToString("N")
$archive = Join-Path ([IO.Path]::GetTempPath()) "tilexr-source-$runId.tar.gz"
$remoteArchive = "/tmp/tilexr-source-$runId.tar.gz"
$oldOutputEncoding = $OutputEncoding
try {
    $OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    Write-Host "Exporting committed HEAD only; uncommitted changes are not included."
    Push-Location -LiteralPath $sourceRoot
    try {
        & $Python tools/validation/source.py pack . $archive
        if ($LASTEXITCODE -ne 0) { throw "Source export failed" }
    } finally { Pop-Location }
    & scp -- $archive "${destination}:$remoteArchive"
    if ($LASTEXITCODE -ne 0) { throw "Source upload failed" }
    $remoteScript = @'
set -euo pipefail
archive=$1
target=$2
trap 'rm -f -- "$archive"' EXIT
python3 - "$archive" "$target" <<'PY'
import json, os, pathlib, shutil, sys, tarfile
archive, target = sys.argv[1:]
root = pathlib.Path(target)
if not root.is_absolute() or '..' in root.parts or root == pathlib.Path('/'):
    raise SystemExit('refusing unsafe deployment root')
for part in (root, *root.parents):
    if part.is_symlink():
        raise SystemExit('deployment root must not traverse symlinks')
marker = root / '.tilexr-validation-root'
marker_text = 'TileXR validation deployment\n'
if (root / '.git').exists():
    raise SystemExit('use a deployment directory without .git; do not overwrite a Git checkout')
if root.exists() and (not root.is_dir() or
        (any(root.iterdir()) and (not marker.is_file() or marker.is_symlink() or
                                 marker.read_text() != marker_text))):
    raise SystemExit('destination must be empty or managed by this validation workflow')
for lock in ('.operation.lock', 'node.lock'):
    if (root / 'run/validation' / lock).exists():
        raise SystemExit('validation is active; refusing source refresh: ' + lock)
def safe(name):
    path = pathlib.PurePosixPath(name)
    if (not name or path.is_absolute() or '\\' in name or
            any(p in ('', '.', '..', '.git') or ':' in p for p in name.split('/'))):
        raise SystemExit('unsafe archive path: ' + name)
    result = root
    for part in path.parts:
        result = result / part
        if result.is_symlink():
            raise SystemExit('destination symlink: ' + name)
    return result
with tarfile.open(archive, 'r:gz') as package:
    members = package.getmembers()
    for member in members:
        safe(member.name)
        if not member.isfile() or member.name in ('hostfile', 'validation.local.sh', '.tilexr-validation-root'):
            raise SystemExit('unexpected archive entry: ' + member.name)
    manifest = json.load(package.extractfile('.tilexr-source.json'))
    if set(manifest['files']) | {'.tilexr-source.json'} != {m.name for m in members}:
        raise SystemExit('archive and source manifest disagree')
    previous = root / '.tilexr-source.json'
    if previous.exists():
        old = json.loads(safe('.tilexr-source.json').read_text())
        for name in old['files']:
            stale = safe(name)
            if name not in manifest['files'] and stale.exists():
                if not stale.is_file() or name in ('hostfile', 'validation.local.sh', '.tilexr-validation-root'):
                    raise SystemExit('refusing to remove non-source entry: ' + name)
                stale.unlink()
    root.mkdir(parents=True, exist_ok=True)
    for member in members:
        path = safe(member.name)
        path.parent.mkdir(parents=True, exist_ok=True)
        with package.extractfile(member) as source, path.open('wb') as output:
            shutil.copyfileobj(source, output)
        path.chmod(member.mode & 0o777)
    marker.write_text(marker_text)
print('Source deployed to ' + str(root))
PY
python3 "$target/tools/validation/source.py" verify "$target"
'@
    $remoteCommand = "bash -s -- " + (Quote-Bash $remoteArchive) + " " + (Quote-Bash $RemotePath)
    $remoteScript.Replace("`r`n", "`n") | & ssh -- $destination $remoteCommand
    if ($LASTEXITCODE -ne 0) { throw "Remote extraction or source verification failed" }
    Write-Host "Ready: $destination at $RemotePath"
} finally {
    $OutputEncoding = $oldOutputEncoding
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive }
}
